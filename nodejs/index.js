'use strict';

const net = require('net');
const path = require('path');
const child_process = require('child_process');
const request = require('request-promise-native');


const SERVER_DIR = path.join(__dirname,'daemon');
const SERVER_PATH = path.join(SERVER_DIR,'jsjaild');
const PIPE_BASENAME = '\\\\?\\pipe\\js-jail.';


class JailError extends Error {}
JailError.prototype.name = 'JailError';
exports.JailError = JailError;

/**
 * Something went wrong in the jail process.
 */
class InternalJailError extends JailError {}
InternalJailError.prototype.name = "InternalJailError";
exports.InternalJailError = InternalJailError;

/** The request was invalid or had the wrong format. */
class RequestJailError extends JailError {}
RequestJailError.prototype.name = "RequestJailError";
exports.RequestJailError = RequestJailError;

/** The result was invalid or had the wrong format (somehow). */
class ResultJailError extends JailError {}
ResultJailError.prototype.name = "ResultJailError";
exports.ResultJailError = ResultJailError;

/** The jail process didn't have enough memory to complete an action. */
class MemoryJailError extends InternalJailError {}
MemoryJailError.prototype.name = "MemoryJailError";
exports.MemoryJailError = MemoryJailError;

/** An error occurred with socket or pipe connection to the jail process. */
class DisconnectJailError extends JailError {}
DisconnectJailError.prototype.name = "DisconnectJailError";
exports.DisconnectJailError = DisconnectJailError;

/** The connection was terminated manually. */
class ClosedJailError extends DisconnectJailError {}
DisconnectJailError.prototype.name = "ClosedJailError";
exports.ClosedJailError = ClosedJailError;


/** The jailed code threw an uncaught exception. */
class ClientError extends Error {}
ClientError.prototype.name = "ClientError";
exports.ClientError = ClientError;

/**
 * A connection to the jail process.
 * 
 * This class does not have a browser-side equivalent. For client-server
 * compatible code, use JailContext instead.
 */
class JailConnection {
    constructor() {
        this._server = null;
        this._pendingRequests = [];
        this._requestQueue = [];
        
        // the rest of the initialization waits until the jail process starts
        if(JailConnection._proc_ready) {
            this._init_connection();
        } else {
            JailConnection._pending_init.push(this);
            
            if(!JailConnection._server_proc) {
                //console.log('starting jail process');

                let options = {
                    stdio: ['ignore','pipe','inherit'],
                    cwd: SERVER_DIR,
                    windowsHide: true
                };
                if(JailConnection._uid !== null) options.uid = JailConnection._uid;
                if(JailConnection._gid !== null) options.gid = JailConnection._gid;

                JailConnection._server_proc = child_process.spawn(
                    SERVER_PATH,
                    [JailConnection._getSocketName()],
                    options);
                
                JailConnection._server_proc.stdout.once('data',(data) => {
                    //console.log('jail process ready');
                    
                    JailConnection._proc_ready = true;
                    for(let inst of JailConnection._pending_init) inst._init_connection();
                    JailConnection._pending_init = null;
                });
            }
        }
    }
    
    static _getSocketName() {
        if(!JailConnection._socketName) {
            JailConnection._socketName = process.platform == 'win32' ?
                    PIPE_BASENAME + require('process').pid :
                    require('tmp').tmpNameSync({template: '/tmp/jsjail-socket.XXXXXX'});
        }
        return JailConnection._socketName;
    }
    
    _init_connection() {
        this._server = net.createConnection(JailConnection._getSocketName());
        this._server.setEncoding('utf8');
        
        this._server.on('error',(e) => {
            this._server = null;
            if(!(e instanceof JailError)) e = new DisconnectJailError(e.message);
 
            for(let r of this._requestQueue) r[1](e);
            this._requestQueue = null;
        });
        
        let buffer = '';
        this._server.on('data',(data) => {
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
            switch(msg.errtype) {
            case 'request': reject(new RequestJailError(msg.message)); break;
            case 'internal': reject(new InternalJailError(msg.message)); break;
            case 'memory': reject(new MemoryJailError(msg.message)); break;
            default: reject(new ResultJailError('unknown error type')); break;
            }
            break;
        default:
            reject(new ResultJailError('unknown result type'));
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

    /**
     * Return a promise to create a new JavaScript context and return its ID.
     */
    createContext() {
        return this.request('{"type":"createcontext"}');
    }

    /**
     * Return a promise to Destroy a previously created context.
     */
    destroyContext(context) {
        return this.request({type: "destroycontext",context: context});
    }

    /**
     * Return a promise to evaluate a string containing JavaScript code and
     * return the result.
     */
    eval(code,context=null) {
        let msg = {type: "eval",code: code};
        if(context !== null) msg.context = context;
        return this.request(msg);
    }

    /**
     * Return a promise to execute a string containing JavaScript code.
     */
    exec(code,context=null) {
        let msg = {type: "exec",code: code};
        if(context !== null) msg.context = context;
        return this.request(msg);
    }

    /**
     * Return a promise to execute a JavaScript function.
     */
    call(func,args=[],context=null) {
        let msg = {type: "call",func: func,args: args};
        if(context !== null) msg.context = context;
        return this.request(msg);
    }

    /**
     * Return a promise to execute a remote JavaScript file.
     */
    execURI(uri,context=null) {
        return request(uri).then((code) => {
            let msg = {type: "exec",code: code};
            if(context !== null) msg.context = context;
            return this.request(msg);
        });
    }

    /**
     * Destroy the connection to the jail process.
     * 
     * This will cause pending requests to be rejected with an instance of
     * ClosedJailError.
     */
    close() {
        const e = new ClosedJailError('the connection has been destroyed');
        if(this._server) {
            this._server.destroy(e);
            this._server = null;
        } else if(this._pendingRequests !== null) {
            for(let p of this._pendingRequests) p[1][1](e);
            this._pendingRequests = null;
            delete JailConnection._pending_init[JailConnection._pending_init.indexOf(this)];
        }
    }

    isPending() {
        return (this._server ? this._requestQueue : this._pendingRequests).length != 0;
    }
}

JailConnection._socketName = null;
JailConnection._proc_ready = false;
JailConnection._server_proc = null;
JailConnection._pending_init = [];
JailConnection._uid = null;
JailConnection._gid = null;

exports.JailConnection = JailConnection;


function setProcUser(uid=null,gid=null) {
    JailConnection._uid = uid;
    JailConnection._gid = gid;
}
exports.setProcUser = setProcUser;


/**
 * If JailConnection launched a process, this will stop it. Otherwise this
 * has no effect.
 * 
 * Creating a new instance of JailConnection after calling shutdown() will
 * re-launch the process.
 */
function shutdown() {
    if(JailConnection._server_proc)
        JailConnection._server_proc.kill('SIGINT');

    JailConnection._proc_ready = false;
    JailConnection._server_proc = null;
    JailConnection._pending_init = [];
}
exports.shutdown = shutdown;


class JailContext {
    constructor(autoClose=false) {
        this._connection = new JailConnection(autoClose);
        this._pendingRequests = [];
        this._contextId = null;

        this._connection.createContext().then(id => {
            this._contextId = id;
            for(let r of this._pendingRequests)
                this._dispatchRequest(r[0]).then(r[1][0],r[1][1]);
            this._pendingRequests = null;
        });
    }

    get autoClose() { return this._connection.autoClose; }
    set autoClose(val) { this._connection.autoClose = val; }

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

    /**
     * Return a promise to evaluate a string containing JavaScript code and
     * return the result.
     */
    eval(code) {
        return this._request({type: "eval",code: code});
    }
    
    /**
     * Return a promise to execute a string containing JavaScript code.
     */
    exec(code) {
        return this._request({type: "exec",code: code});
    }
    
    /**
     * Return a promise to execute a JavaScript function.
     */
    call(func,args=[]) {
        return this._request({type: "call",func: func,args: args});
    }
    
    /**
     * Return a promise to execute a remote JavaScript file.
     */
    execURI(uri) {
        return request(uri).then((code) => {
            return this._request({type: "exec",code: code});
        });
    }
    
    /**
     * Destroy the connection to the jail process.
     * 
     * This will cause pending requests to be rejected with an instance of
     * ClosedJailError.
     */
    close() {
        this._connection.close();
    }
}
exports.JailContext = JailContext;


function evalJailed(code) {
    return (new JailConnection(true)).eval(code);
}
exports.evalJailed = evalJailed;


