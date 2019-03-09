
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

const DEFAULT_CACHE_QTY = 50;

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

function isSomething(x) { return x !== null && x !== undefined; }


/* A map that keeps track of which item was accessed the least recently */
class LastAccessMap {
    _removeNode(n) {
        if(n[0]) n[0][1] = n[1];
        else this._head = n[1];
        if(n[1]) n[1][0] = n[0];
        else this._tail = n[0];
    }

    constructor() {
        this._head = null;
        this._tail = null;
        this._map = new Map();
    }

    _getNode(key) {
        let node = this._map.get(key);
        if(node === undefined) return undefined;
        this._removeNode(node);
        node[0] = this._tail;
        node[1] = null;
        if(this._tail) this._tail[1] = node;
        this._tail = node;

        return node;
    }

    get(key) {
        let node = this._getNode(key);
        return node && node[3];
    }

    set(key,value) {
        let node = this._getNode(key);
        if(node === undefined) {
            node = [this._tail,null,key,value];
            if(this._tail) this._tail[1] = node;
            else this._head = node;
            this._tail = node;
            this._map.set(key,node);
        } else {
            node[3] = value;
        }
    }

    delete(key) {
        let node = this._map.get(key);
        if(node !== undefined) {
            this._map.delete(key);
            this._removeNode(node);
        }
    }

    deleteLast() {
        if(this._head === null) throw new RangeError('no items to delete');
        let tmp = this._head;
        this._head = tmp[1];
        if(this._head !== null) this._head[0] = null;
        else this._tail = null;
        this._map.delete(tmp[2]);
    }

    clear() {
        this._head = null;
        this._tail = null;
        this._map.clear();
    }

    get size() { return this._map.size; }
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

    static _backgroundError(msg) {
        console.error(msg);
    }

    constructor(autoClose=false) {
        this.autoClose = autoClose;
        this._frameReady = false;
        this._id = JailContext.uniqueId();

        this._pendingRequests = [];
        this._asyncRequests = new Map();

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

    async _import(mid) {
        let m = null;
        try {
            m = JailContext._modCache.get(mid);
            if(m === undefined) {
                m = await JailContext._moduleLoader(mid);
                if(m === null) throw new Error('module not found');
                if(m instanceof ArrayBuffer) m = await WebAssembly.compile(m);
                else if(!(typeof m == "string")) throw new Error('imported data must be instance of ArrayBuffer or String');
                JailContext._modCache.set(mid,m);
                while(JailContext._modCache.size > JailContext._maxCacheQty) {
                    JailContext._modCache.deleteLast();
                }
            }
        } catch(e) {
            this._dispatch_module_error(null,mid,e);
            return;
        }

        this._frame.contentWindow.postMessage({
            type: 'module',
            mid: mid,
            value: m
        },'*');
    }

    async _normalizeName(name) {
        let mid;
        try {
            mid = await JailContext._moduleID(name);
            if(mid === null) throw new Error('module not found');
        } catch(e) {
            this._dispatch_module_error(name,null,e);
            return;
        }

        this._frame.contentWindow.postMessage({
            type: 'modulename',
            name: name,
            mid: mid
        },"*");
    }

    _dispatch_result(msg) {
        let resolve;
        switch(msg.type) {
        case 'result':
            this._requestQueue.shift()[0](msg.value);
            break;
        case 'success':
            this._requestQueue.shift()[0]();
            break;
        case 'resultexception':
            this._requestQueue.shift()[1](new ClientError(msg.message));
            break;
        case 'asyncresult':
        case 'asyncresultexception':
            resolve = this._asyncRequests.get(msg.aid);
            if(resolve === undefined) JailContext._backgroundError('received results with ID not matching any request');
            else {
                this._asyncRequests.delete(msg.aid);
                if(msg.type == 'asyncresult') resolve[0](msg.value);
                else resolve[1](new ClientError(msg.message));
            }
            break;
        case 'resultpending':
            this._asyncRequests.set(msg.aid,this._requestQueue.shift());
            break;
        case 'normalizename':
            this._normalizeName(msg.name);
            break;
        case 'import':
            this._import(msg.mid);
            break;
        case 'error':
            JailContext._backgroundError(msg.message);
            break;
        default:
            JailContext._backgroundError('unknown result type');
            break;
        }

        if(this.autoClose &&
            this._requestQueue.length == 0 &&
            this._asyncRequests.size == 0) this.close();
    }

    _dispatch_module_error(name,mid,e) {
        this._frame.contentWindow.postMessage({
            type: 'moduleerror',
            name: name,
            mid: mid,
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

    call(func,args=[],resolve_async=false) {
        return this._request({type: "call",func: func,args: args,async: resolve_async});
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
        return Promise.resolve({
            connections: JailContext._jails.size,
            cacheitems: JailContext._modCache.size
        });
    }
}

const defaultLoader = mid => null;
const defaultModID = x => x;
JailContext._idCount = 0;
JailContext._jails = new Map();
JailContext._modCache = new LastAccessMap();
JailContext._moduleLoader = defaultLoader;
JailContext._moduleID = defaultModID;
JailContext._maxCacheQty = DEFAULT_CACHE_QTY;

function setModuleLoader(loader,getID=null,maxCacheQty=null) {
    JailContext._moduleLoader = loader || defaultLoader;
    JailContext._moduleID = getID || defaultModID;
    JailContext._maxCacheQty = isSomething(maxCacheQty) ? maxCacheQty : DEFAULT_CACHE_QTY;
}

function purgeCache(items=null) {
    if(isSomething(items)) {
        for(let item of items) JailContext._modCache.delete(item);
    } else {
        JailContext._modCache.clear();
    }
}


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
    JailContext,
    setModuleLoader,
    purgeCache
};

}));
