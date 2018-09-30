'use strict';

const net = require('net');
const path = require('path');
const child_process = require('child_process');
const request = require('request-promise-native');


// #if !this.NODEONLY
const SERVER_DIR = path.join(__dirname,'daemon');
const SERVER_PATH = path.join(SERVER_DIR,'jsjaild');
// #endif
const PIPE_BASENAME = '\\\\?\\pipe\\js-jail.';


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
DisconnectJailError.prototype.name = "ClosedJailError";
exports.ClosedJailError = ClosedJailError;

class ContextJailError extends JailError {}
ContextJailError.prototype.name = "ContextJailError";
exports.ContextJailError = ContextJailError;


class ClientError extends Error {}
ClientError.prototype.name = "ClientError";
exports.ClientError = ClientError;


class JailConnection {
    constructor(autoClose=false) {
        this.autoClose = autoClose;
        this._server = null;
        this._pendingRequests = [];
        this._requestQueue = [];

        // the rest of the initialization waits until the jail process starts
        if(JailConnection._proc_ready) {
            this._init_connection();
        } else {
            JailConnection._pending_init.add(this);

            if(!JailConnection._server_proc) {
                //console.log('starting jail process');

                let options = {
// #if this.NODEONLY
                    stdio: ['ignore','pipe','inherit','ipc'],
                    execArgv: ['--experimental-worker'],
// #else
                    stdio: ['ignore','pipe','inherit'],
                    cwd: SERVER_DIR,
// #endif
                    windowsHide: true
                };
                if(JailConnection._uid !== null) options.uid = JailConnection._uid;
                if(JailConnection._gid !== null) options.gid = JailConnection._gid;

// #if this.NODEONLY
                JailConnection._server_proc = child_process.fork(
                    path.join(__dirname,'jailed.js'),
// #else
                JailConnection._server_proc = child_process.spawn(
                    SERVER_PATH,
                    [JailConnection._getSocketName()],
// #endif
                    options);

                JailConnection._server_proc.stdout.once('data',(data) => {
                    //console.log('jail process ready');

                    JailConnection._proc_ready = true;
                    JailConnection._pending_init.forEach(inst => { inst._init_connection(); });
                    JailConnection._pending_init = null;
                });
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

        let buffer = '';
        this._server.on('data',data => {
            let results = data.split('\x00').reverse();
            while(results.length) {
                buffer += results.pop();
                if(results.length > 0) {
                    /* reset the buffer and remove the callback from the queue
                       before doing anything that can fail */
                    const message = buffer;
                    buffer = '';
                    const callbacks = this._requestQueue.shift();

                    this._dispatch_result(JSON.parse(message),callbacks[0],callbacks[1]);
                }
            }
        });

        for(let [req,callbacks] of this._pendingRequests) this._dispatch_request(req,callbacks);
        this._pendingRequests = [];
    }

    _dispatch_result(msg,resolve,reject) {
        let errC;
        switch(msg.type) {
        case 'result':
            resolve(msg.value);
            break;
        case 'success':
            resolve(msg.context);
            break;
        case 'resultexception':
            reject(new ClientError(msg.message));
            break;
        case 'error':
            if(msg.errtype == 'context') errC = ContextJailError;
            else errC = InternalJailError;
            reject(new errC(msg.message));
            break;
        default:
            reject(new InternalJailError('unknown result type'));
            break;
        }

        if(this.autoClose && this._requestQueue.length == 0) this.close();
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

    call(func,args=[],context=null) {
        let msg = {type: "call",func: func,args: args};
        if(context !== null) msg.context = context;
        return this.request(msg);
    }

    execURI(uri,context=null) {
        return request(uri).then((code) => {
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
            JailConnection._pending_init.delete(this);
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

exports.JailConnection = JailConnection;


function setProcUser(uid=null,gid=null) {
    JailConnection._uid = uid;
    JailConnection._gid = gid;
}
exports.setProcUser = setProcUser;


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

    call(func,args=[]) {
        return this._request({type: "call",func: func,args: args});
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

