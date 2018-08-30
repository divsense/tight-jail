'use strict';

const net = require('net');
const child_process = require('child_process');


const SERVER_PATH = 'jsjaild';
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


/** The jailed code threw an uncaught exception. */
class ClientError extends Error {}
ClientError.prototype.name = "ClientError";
exports.ClientError = ClientError;

class JSJail {
    constructor() {
        this._server = null;
        this._pendingRequests = [];
        this._requestQueue = [];
        
        // the rest of the initialization waits until the sandbox process starts
        if(this._proc_ready) {
            this._init_connection();
        } else {
            JSJail._pending_init.push(this);
            
            if(!JSJail._server_proc) {
                //console.log('starting jail process');
                
                JSJail._server_proc = child_process.spawn(
                    SERVER_PATH,
                    [JSJail._getSocketName()],
                    {
                        stdio: ['ignore','pipe','inherit'],
                        windowsHide: true
                    });
                
                JSJail._server_proc.stdout.once('data',(data) => {
                    //console.log('jail process ready');
                    
                    JSJail._proc_ready = true;
                    for(let inst of JSJail._pending_init) inst._init_connection();
                    JSJail._pending_init = null;
                });
            }
        }
    }
    
    static _getSocketName() {
        if(!JSJail._socketName) {
            JSJail._socketName = process.platform == 'win32' ?
                    PIPE_BASENAME + require('process').pid :
                    require('tmp').tmpNameSync({template: '/tmp/jsjail-socket.XXXXXX'});
        }
        return JSJail._socketName;
    }
    
    _init_connection() {
        this._server = net.createConnection(JSJail._getSocketName());
        this._server.setEncoding('utf8');
        
        let self = this;
        
        this._server.on('error',(e) => {
            self._server = null;
            while(self._requestQueue.length) {
                self._requestQueue.shift()[1](new DisconnectJailError(e.message))
            }
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
                    const callbacks = self._requestQueue.shift();
                    
                    self._dispatch_result(JSON.parse(message),callbacks[0],callbacks[1]);
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
        let self = this;
        return new Promise(
            (resolve,reject) => {
                if(JSJail._proc_ready) self._dispatch_request(req,[resolve,reject]);
                else self._pendingRequests.push([req,[resolve,reject]]);
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
     * If JSJail launched a process, this will stop it. Otherwise this has no
     * effect.
     * 
     * Creating a new instance of JSJail after calling shutdown() will re-launch
     * the process.
     */
    static shutdown() {
        if(JSJail._server_proc)
            JSJail._server_proc.kill('SIGINT');
        
        JSJail._proc_ready = false;
        JSJail._server_proc = null;
        JSJail._pending_init = [];
    }
}

JSJail._socketName = null;
JSJail.shutdown(); // this sets some static attributes

exports.JSJail = JSJail;

