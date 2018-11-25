
// universal module definition
(function (root,factory) {
    if(typeof define === 'function' && define.amd) {
        define([], factory);
    } else if(typeof exports === 'object') {
        module.exports = factory();
    } else {
        root.tightjail = factory();
    }
}(typeof self !== 'undefined' ? self : this,() => {

class JailError extends Error {}
JailError.prototype.name = 'JailError';

class InternalJailError extends JailError {}
InternalJailError.prototype.name = "InternalJailError";

class DisconnectJailError extends JailError {}
DisconnectJailError.prototype.name = "DisconnectJailError";

class ClosedJailError extends DisconnectJailError {}
DisconnectJailError.prototype.name = "ClosedJailError";


class ClientError extends Error {}
ClientError.prototype.name = "ClientError";

class Importer {
    load(name) { return null; }

    constructor() {
        this._modCache = new Map();
    }
}

class JailContext {
    static uniqueId() {
        /* a random number is tacked on to prevent anyone from relying on the ID
        to have any particular value */
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

    constructor(autoClose=false,importer=null) {
        this.autoClose = autoClose;
        this.importer = importer || new Importer();
        this._frameReady = false;
        this._id = JailContext.uniqueId();

        this._pendingRequests = [];

        // wait for a message from the frame before sending any requests
        this._requestQueue = [[
            () => {
                this._frameReady = true;
                for(let [req,callbacks] of this._pendingRequests)
                    this._dispatch_request(req,callbacks);
                this._pendingRequests = null;
            },
            () => {}]];

        this._frame = JailContext.createFrame();
        this._frame.setAttribute("src","jailed.html#" + this._id);
        document.body.appendChild(this._frame);
        JailContext._jails.set(this._id,this);
    }

    async _import(name) {
        let m = null;
        try {
            m = this.importer._modCache.get(name);
            if(m === undefined) {
                m = await this.importer.load(msg.name);
                if(m instanceof ArrayBuffer) m = await WebAssembly.compile(m);
                else if(!m typeof "string") throw new Error('imported data must be instance of ArrayBuffer or String');
                this.importer._modCache.set(name,m);
            }
        } catch(e) {
            this._dispatch_module_error(name,e);
            return;
        }
        this._dispatch_module(name,m);
    }

    _dispatch_result(msg) {
        if(msg.type == 'import') this._import(msg.name);
        else {
            let [resolve,reject] = jail._requestQueue.shift();
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
                reject(new InternalJailError(msg.message));
                break;
            case 'import':
                break;
            default:
                reject(new InternalJailError('unknown result type'));
                break;
            }

            if(this.autoClose && this._requestQueue.length == 0) this.close();
        }
    }

    _dispatch_module(name,m) {
        this._frame.contentWindow.postMessage({
            type: 'module',
            name: name,
            value: m
        },"*");
    }

    _dispatch_module_error(name,e) {
        this._frame.contentWindow.postMessage({
            type: 'moduleerror',
            name: name,
            message: e.message
        },"*");
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

    eval(code) {
        return this._request({type: "eval",code: code});
    }

    exec(code) {
        return this._request({type: "exec",code: code});
    }

    call(func,args=[]) {
        return this._request({type: "call",func: func,args: args});
    }

    execURI(uri,context=null) {
        return this._request({type: "execuri",uri: uri});
    }

    close() {
        if(this._frame) {
            JailContext._jails.delete(this._id);
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

    getStats() {
        return Promise.resolve({connections: JailContext._jails.size});
    }
}

JailContext._idCount = 0;
JailContext._jails = new Map();
JailContext._modCache = new Map();


window.addEventListener("message",(event) => {
    const jail = JailContext._jails.get(event.data.id);

    /* this can happen if an iframe was removed before its code finished */
    if(jail === undefined) return;

    jail._dispatch_result(event.data);
},false);

return {
    JailError,
    InternalJailError,
    DisconnectJailError,
    ClosedJailError,
    ClientError,
    JailContext};

}));