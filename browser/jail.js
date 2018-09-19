
class JailError extends Error {}
JailError.prototype.name = 'JailError';

/**
 * Something went wrong in the jail IFRAME.
 */
class InternalJailError extends JailError {}
InternalJailError.prototype.name = "InternalJailError";

/** The request was invalid or had the wrong format. */
class RequestJailError extends JailError {}
RequestJailError.prototype.name = "RequestJailError";

/** The result was invalid or had the wrong format (somehow). */
class ResultJailError extends JailError {}
ResultJailError.prototype.name = "ResultJailError";

/** The connection to the jail was lost. */
class DisconnectJailError extends JailError {}
DisconnectJailError.prototype.name = "DisconnectJailError";

/** The connection was terminated manually. */
class ClosedJailError extends DisconnectJailError {}
DisconnectJailError.prototype.name = "ClosedJailError";


/** The jailed code threw an uncaught exception. */
class ClientError extends Error {}
ClientError.prototype.name = "ClientError";

class JailContext {
    static uniqueId() {
        /* a random number is tacked on to prevent anyone from relying on the ID to
        have any particular value */
        var r = JailContext._idCount + "." + Math.random();
        JailContext._idCount += 1;
        return r;
    }

    static createFrame() {
        var f = document.createElement("iframe");
        f.sandbox = "allow-scripts allow-same-origin";
        f.style.display = "none";
        return f;
    }
    
    constructor(autoClose=false) {
        this.autoClose = autoClose;
        this._frameReady = false;
        this._id = JailContext.uniqueId();
        
        this._pendingRequests = [];
        
        // wait for a message from the frame before sending any requests
        this._requestQueue = [[
            () => {
                this._frameReady = true;
                for(let [req,callbacks] of this._pendingRequests) this._dispatch_request(req,callbacks);
                this._pendingRequests = null;
            },
            () => {}]];
        
        this._frame = JailContext.createFrame();
        this._frame.setAttribute("src","jailed.html#" + this._id);
        document.body.appendChild(this._frame);
        JailContext._jails[this._id] = this;
    }
    
    _dispatch_result(msg,resolve,reject) {
        switch(msg.type) {
        case 'result':
            resolve(msg.value);
            break;
        case 'success':
            resolve();
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
        this._requestQueue.push(callbacks);
        this._frame.contentWindow.postMessage(req,"*");
    }

    _request(req) {
        return new Promise(
            (resolve,reject) => {
                if(this._frameReady) this._dispatch_request(req,[resolve,reject]);
                else this._pendingRequests.push([req,[resolve,reject]]);
            });
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
    execURI(uri,context=null) {
        return this._request({type: "execuri",uri: uri});
    }
    
    /**
     * Destroy the jail.
     * 
     * This will cause pending requests to be rejected with an instance of
     * ClosedJailError.
     */
    close() {
        if(this._frame) {
            delete JailContext._jails[this._id];
            this._frame.parentElement.removeChild(this._frame);
            this._frame = null;
            const e = new ClosedJailError('the connection has been destroyed');
            if(this._pendingRequests) {
                for(let p of this._pendingRequests) p[1][1](e);
                this._pendingRequests = null;
            }
            for(let r of this._requestQueue) r[1](e);
            this._requestQueue = null;
        }
    }
}

JailContext._idCount = 0;
JailContext._jails = {};


window.addEventListener("message",(event) => {
    const jail = JailContext._jails[event.data.id];
    
    /* this can happen if an iframe was removed before its code finished */
    if(jail === undefined) return;
    
    const callbacks = jail._requestQueue.shift();
    jail._dispatch_result(event.data,callbacks[0],callbacks[1]);
},false);


function evalJailed(code) {
    return (new JailContext(true)).eval(code);
}

