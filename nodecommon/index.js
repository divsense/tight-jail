'use strict';

const fs = require('fs');
const net = require('net');
const path = require('path');
const child_process = require('child_process');
const request = require('request-promise-native');
const dlist = require('yallist');


// #if !this.NODEONLY
const SERVER_DIR = path.join(__dirname,'daemon');
const SERVER_PATH = path.join(SERVER_DIR,'jsjaild');
// #endif
const PIPE_BASENAME = '\\\\?\\pipe\\js-jail.';
const DEFAULT_CACHE_QTY = 50;


class JailError extends Error {}
JailError.prototype.name = 'JailError';
exports.JailError = JailError;

class InternalJailError extends JailError {}
InternalJailError.prototype.name = "InternalJailError";
exports.InternalJailError = InternalJailError;

class DisconnectJailError extends JailError {}
DisconnectJailError.prototype.name = "DisconnectJailError";
exports.DisconnectJailError = DisconnectJailError;

class ClosedJailError extends DisconnectJailError {}
ClosedJailError.prototype.name = "ClosedJailError";
exports.ClosedJailError = ClosedJailError;

class ContextJailError extends JailError {}
ContextJailError.prototype.name = "ContextJailError";
exports.ContextJailError = ContextJailError;


class ClientError extends Error {}
ClientError.prototype.name = "ClientError";
exports.ClientError = ClientError;

function isSomething(x) { return x !== null && x !== undefined; }

/* A combination FIFO/set.

Adding the same value again pushes it to the back. */
class UniqueFIFO {
    constructor() {
        this._list = dlist.create();
        this._map = new Map();
    }

    enqueue(value) {
        let node = this._map.get(value);
        if(node === undefined) {
            this._list.push(value);
            this._map.set(value,this._list.tail);
            return true;
        }

        this._list.pushNode(node);
        return false;
    }

    dequeue() {
        if(!this._map.size) throw new RangeError('no items to dequeue');
        let value = this._list.shift();
        this._map.delete(value);
        return value;
    }

    has(value) {
        return this._map.get(value) !== undefined;
    }

    delete(value) {
        let item = this._map.get(value);
        if(item !== undefined) {
            this._map.delete(value);
            this._list.removeNode(item);
        }
    }

    clear() {
        this._list = dlist.create();
        this._map.clear();
    }

    get size() { return this._map.size; }
}

class JailConnection {
    /* This is for background errors that should never occur, and should
    not be ignored. */
    static _backgroundError(msg) {
        console.error(msg);
        shutdown();
    }

    static _processChunks(callback) {
        let buffer = '';
        return data => {
            let results = data.split('\x00').reverse();
            while(results.length) {
                buffer += results.pop();
                if(results.length > 0) {
                    /* reset the buffer before doing anything that can fail */
                    const message = buffer;
                    buffer = '';
                    callback(message);
                }
            }
        };
    }

    static _importError(data,err) {
        let msg = {
            connection: data.connection,
            context: data.context,
            name: data.name,
            type: 'moduleerror',
            msg: err.toString()
        };
        JailConnection._server_proc.stdin.write(JSON.stringify(msg) + '\0');
    }

    static _importNotFound(data) { JailConnection._importError(data,'module not found'); }

    static _purgeCache(items=null) {
        let msg = {
            type: 'purgecache',
            ids: items
        };

        JailConnection._server_proc.stdin.write(JSON.stringify(msg) + '\0');
    }

    static async _handleImport(data) {
        let mid;
        try {
            mid = await JailConnection._moduleID(data.name);
        } catch(e) {
            JailConnection._importError(data,e);
            return;
        }

        if(!isSomething(mid)) {
            JailConnection._importNotFound(data);
            return;
        }

        mid = mid.toString();

        let msg = {
            connection: data.connection,
            context: data.context,
            name: data.name,
            id: mid
        };

        if(JailConnection._modCache.has(mid)) {
            msg.type = 'modulecached';
        } else {
            let value;
            try {
                value = await JailConnection._moduleLoader(mid);
            } catch(e) {
                JailConnection._importError(data,e);
                return;
            }

            if(!isSomething(value)) {
                JailConnection._importNotFound(data);
                return;
            }

            msg.type = 'module';
            if(typeof value == 'string') {
                msg.modtype = 'js';
                msg.value = value;
            } else {
                if(!(value instanceof Buffer)) {
                    JailConnection._importError('module data must be a string or Buffer');
                    return;
                }
                msg.modtype = 'wasm';
                msg.value = value.toString('base64');
            }
        }

        /* even if the ID is already in the cache, it needs to be enqueued again
        to move it to the back of the queue */
        JailConnection._modCache.enqueue(mid);

        JailConnection._server_proc.stdin.write(JSON.stringify(msg) + '\0');

        let excess = [];
        while(JailConnection._modCache.size > JailConnection._maxCacheQty) {
            excess.push(JailConnection._modCache.dequeue());
        }
        if(excess.length) JailConnection._purgeCache(excess);
    }

    static _setupProc() {
        JailConnection._server_proc.stdout.setEncoding('utf8');

        JailConnection._server_proc.stdout.on('data',JailConnection._processChunks(msg => {
            let data;
            try {
                data = JSON.parse(msg);
            } catch(e) {
                JailConnection._backgroundError('mangled result in importer stream');
                return;
            }

            if(JailConnection._proc_ready) {
                switch(data.type) {
                case 'import':
                    JailConnection._handleImport(data);
                    break;
                case 'error':
                    JailConnection._backgroundError('error in importer stream: ' + data.message);
                    break;
                default:
                    JailConnection._backgroundError('unknown result type in importer stream');
                    break;
                }
            } else {
                //console.log('jail process ready');

                console.assert(data.type == 'ready');
                JailConnection._proc_ready = true;
                JailConnection._pending_init.forEach(inst => { inst._init_connection(); });
                JailConnection._pending_init = null;
            }
        }));
    }

    constructor(autoClose=false) {
        this.autoClose = autoClose;
        this._server = null;
        this._pendingRequests = [];
        this._requestQueue = [];
        this._asyncRequests = new Map();

        // the rest of the initialization waits until the jail process starts
        if(JailConnection._proc_ready) {
            this._init_connection();
        } else {
            JailConnection._pending_init.add(this);

            if(!JailConnection._server_proc) {
                if(process.env.JAIL_PROCESS_SOCKET) {
                    JailConnection._socketName = process.env.JAIL_PROCESS_SOCKET;
                    JailConnection._server_proc = {
                        stdin: fs.createWriteStream(JailConnection._socketName + '_stdin'),
                        stdout: fs.createReadStream(JailConnection._socketName + '_stdout'),
                        kill(x) {
                            this.stdin.close();
                            this.stdout.close();
                        }
                    };
                } else {
                    //console.log('starting jail process');

                    let options = {
// #if this.NODEONLY
                        stdio: ['pipe','pipe','inherit','ipc'],
                        execArgv: ['--experimental-worker'],
// #else
                        stdio: ['pipe','pipe','inherit'],
                        cwd: SERVER_DIR,
// #endif
                        windowsHide: true
                    };
                    if(JailConnection._uid !== null) options.uid = JailConnection._uid;
                    if(JailConnection._gid !== null) options.gid = JailConnection._gid;

// #if this.NODEONLY
                    JailConnection._server_proc = child_process.fork(
                        path.join(__dirname,'jailed.js'),
                        options);
// #else
                    JailConnection._server_proc = child_process.spawn(
                        SERVER_PATH,
                        [JailConnection._getSocketName()],
                        options);
// #endif
                }
                JailConnection._setupProc();
            }
        }
    }

    static _getSocketName() {
        if(!JailConnection._socketName) {
            JailConnection._socketName = process.platform == 'win32' ?
                PIPE_BASENAME + process.pid :
                require('tmp').tmpNameSync({template: '/tmp/jsjail-socket.XXXXXX'});
        }
        return JailConnection._socketName;
    }

    _rejectRequests(e) {
        for(let r of this._requestQueue) r[1](e);
        this._requestQueue = null;
    }

    _init_connection() {
        this._server = net.createConnection(JailConnection._getSocketName());
        this._server.setEncoding('utf8');

        this._server.on('error',e => {
            this._server.removeAllListeners('close');
            this._server = null;
            if(!(e instanceof JailError)) e = new DisconnectJailError(e.message);

            this._rejectRequests(e);
        });

        this._server.on('close',() => {
            this._rejectRequests(new DisconnectJailError('connection to jail process closed unexpectedly'));
        });

        this._server.on('data',JailConnection._processChunks(msg => {
            let parsed;
            try {
                parsed = JSON.parse(msg);
            } catch(e) {
                JailConnection._backgroundError('mangled result');
                return;
            }

            this._dispatchResult(parsed);
        }));

        for(let [req,callbacks] of this._pendingRequests) this._dispatch_request(req,callbacks);
        this._pendingRequests = [];
    }

    _dispatchResult(msg) {
        let errC;
        let resolve;
        switch(msg.type) {
        case 'result':
            this._requestQueue.shift()[0](msg.value);
            break;
        case 'success':
            this._requestQueue.shift()[0](msg.context);
            break;
        case 'asyncresult':
        case 'asyncresultexception':
            resolve = this._asyncRequests.get(msg.id);
            if(resolve === undefined) JailConnection._backgroundError('received results with ID not matching any request');
            else {
                this._asyncRequests.delete(msg.id);
                if(msg.type == 'asyncresult') resolve[0](msg.value);
                else resolve[1](new ClientError(msg.message));
            }
            break;
        case 'resultexception':
            this._requestQueue.shift()[1](new ClientError(msg.message));
            break;
        case 'resultpending':
            this._asyncRequests.set(msg.id,this._requestQueue.shift());
            break;
        case 'error':
            if(msg.errtype == 'context') errC = ContextJailError;
            else errC = InternalJailError;
            this._requestQueue.shift()[1](new errC(msg.message));
            break;
        default:
            JailConnection._backgroundError('unknown result type');
            break;
        }

        if(this.autoClose &&
            this._requestQueue.length == 0 &&
            this._asyncRequests.size == 0) this.close();
    }

    _dispatch_request(req,callbacks) {
        if(!this._server) {
            // The socket/pipe had an error. Try to reestablish it.
            this._init_connection();
        }

        this._requestQueue.push(callbacks);
        this._server.write(req + '\0');
    }

    request(req) {
        if(typeof req != 'string') req = JSON.stringify(req);
        return new Promise(
            (resolve,reject) => {
                if(JailConnection._proc_ready) this._dispatch_request(req,[resolve,reject]);
                else this._pendingRequests.push([req,[resolve,reject]]);
            });
    }

    createContext() {
        return this.request('{"type":"createcontext"}');
    }

    destroyContext(context) {
        return this.request({type: "destroycontext",context: context});
    }

    eval(code,context=null) {
        let msg = {type: "eval",code: code};
        if(context !== null) msg.context = context;
        return this.request(msg);
    }

    exec(code,context=null) {
        let msg = {type: "exec",code: code};
        if(context !== null) msg.context = context;
        return this.request(msg);
    }

    call(func,args=[],context=null,resolve_async=false) {
        let msg = {type: "call",func: func,args: args,async: !!resolve_async};
        if(context !== null) msg.context = context;
        return this.request(msg);
    }

    execURI(uri,context=null) {
        return request(uri).then(code => {
            let msg = {type: "exec",code: code};
            if(context !== null) msg.context = context;
            return this.request(msg);
        });
    }

    close(extra_cb=null) {
        const e = new ClosedJailError('the connection has been destroyed');
        if(extra_cb) extra_cb(e);

        if(this._server) {
            this._server.destroy(e);
        } else if(this._pendingRequests !== null) {
            for(let p of this._pendingRequests) p[1][1](e);
            this._pendingRequests = null;
            if(JailConnection._pending_init) JailConnection._pending_init.delete(this);
        }
    }

    getStats() {
        return this.request('{"type":"getstats"}');
    }

    isPending() {
        return (this._server ? this._requestQueue : this._pendingRequests).length != 0;
    }
}

JailConnection._socketName = null;
JailConnection._proc_ready = false;
JailConnection._server_proc = null;
JailConnection._pending_init = new Set();
JailConnection._uid = null;
JailConnection._gid = null;

const defaultLoader = mid => null;
const defaultModID = x => x;
JailConnection._moduleLoader = defaultLoader;
JailConnection._moduleID = defaultModID;
JailConnection._modCache = new UniqueFIFO();
JailConnection._maxCacheQty = DEFAULT_CACHE_QTY;

exports.JailConnection = JailConnection;


function setProcUser(uid=null,gid=null) {
    JailConnection._uid = uid;
    JailConnection._gid = gid;
}
exports.setProcUser = setProcUser;

function setModuleLoader(loader,getID=null,maxCacheQty=null) {
    JailConnection._moduleLoader = loader || defaultLoader;
    JailConnection._moduleID = getID || defaultModID;
    JailConnection._maxCacheQty = isSomething(maxCacheQty) ? maxCacheQty : DEFAULT_CACHE_QTY;
}
exports.setModuleLoader = setModuleLoader;

function purgeCache(items=null) {
    if(!JailConnection._server_proc) return;

    if(isSomething(items)) {
        items = Array.from(items,x => x.toString());
        if(!items.length) return;
        for(let item of items) JailConnection._modCache.delete(item);
    } else {
        items = null;
        JailConnection._modCache.clear();
    }

    JailConnection._purgeCache(items);
}
exports.purgeCache = purgeCache;

function shutdown() {
    if(JailConnection._server_proc) {
        JailConnection._server_proc.kill('SIGINT');
        if(JailConnection._pending_init)
            JailConnection._pending_init.forEach(inst => { inst.close(); });
    }

    JailConnection._proc_ready = false;
    JailConnection._server_proc = null;
    JailConnection._pending_init = new Set();
}
exports.shutdown = shutdown;


class JailContext {
    constructor(autoClose=false) {
        this._autoClose = autoClose;
        this._connection = new JailConnection();
        this._pendingRequests = [];
        this._contextId = null;

        this._connection.createContext().then(id => {
            this._contextId = id;
            for(let r of this._pendingRequests)
                this._dispatchRequest(r[0]).then(r[1][0],r[1][1]);
            this._pendingRequests = null;
            this._connection.autoClose = this._autoClose;
        },e => {
            this._rejectPending(e);
        });
    }

    _rejectPending(e) {
        if(this._pendingRequests) {
            for(let r of this._pendingRequests) r[1][1](e);
            this._pendingRequests = null;
        }
    }

    _dispatchRequest(req) {
        req.context = this._contextId;
        return this._connection.request(req);
    }

    _request(req) {
        if(this._contextId === null)
            return new Promise((resolve,reject) => {
                this._pendingRequests.push([req,[resolve,reject]]);
            });
        else
            return this._dispatchRequest(req);
    }

    eval(code) {
        return this._request({type: "eval",code: code});
    }

    exec(code) {
        return this._request({type: "exec",code: code});
    }

    call(func,args=[],resolve_async=false) {
        return this._request({type: "call",func: func,args: args,async: !!resolve_async});
    }

    execURI(uri) {
        return request(uri).then(code => {
            return this._request({type: "exec",code: code});
        });
    }

    close() {
        this._connection.close(e => { this._rejectPending(e); });
    }

    getStats() { return this._connection.getStats(); }
}
exports.JailContext = JailContext;

