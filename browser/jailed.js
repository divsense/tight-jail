'use strict';

class __PendingMod {
    constructor(name,resolve1,reject1) {
        this.callbacks = [[resolve1,reject1]];
        this.aliases = [name];
    }

    add(resolve,reject) {
        this.callbacks.push([resolve,reject]);
    }

    mergeFrom(b) {
        for(let c of b.callbacks) this.callbacks.push(c);
        for(let a of b.aliases) this.aliases.push(a);
    }

    resolve(x) {
        for(let a of this.aliases) __moduleCache[a] = x;
        for(let c of this.callbacks) c[0](x);
    }

    reject(x) {
        for(let a of this.aliases) delete __moduleCache[a];
        for(let c of this.callbacks) c[1](x);
    }
}

function __modEval(__code) {
    var exports = {};
    eval(__code);
    return exports;
}

class JailImportError extends Error {}
JailImportError.prototype.name = 'JailImportError';

const __moduleCache = {};
const __moduleCacheById = {};
let __nextAsyncID = 1;

onmessage = (event) => {
    /* calling eval indirectly will cause it to execute in global scope, which
       is what we want */
    const eval_ = eval;

    async function onmodulemessage(data) {
        let p = __moduleCacheById[data.mid];
        console.assert(p instanceof __PendingMod);
        try {
            let v;
            if(data.value instanceof WebAssembly.Module) {
                v = (await WebAssembly.instantiate(data.value)).exports;
            } else {
                v = __modEval(data.value);
            }

            __moduleCacheById[data.mid] = v;
            p.resolve(v);
        } catch(e) {
            delete __moduleCacheById[data.mid];
            p.reject(new JailImportError(e.toString()));
        }
    }

    function resultException(e) {
        postMessage({
            type: 'resultexception',
            message: e.message,
            filename: e.filename,
            lineno: e.lineNumber});
    }

    try {
        let r, byName;
        switch(event.data.type) {
        case 'eval':
            try {
                r = eval_(event.data.code);
            } catch(e) {
                resultException(e);
                break;
            }
            postMessage({type: 'result',value: r});
            break;
        case 'exec':
            try {
                eval_(event.data.code);
            } catch(e) {
                resultException(e);
                break;
            }
            postMessage({type: 'success'});
            break;
        case 'execuri':
            try {
                importScripts(event.data.uri);
            } catch(e) {
                resultException(e);
                break;
            }
            postMessage({type: 'success'});
            break;
        case 'call':
            try {
                r = self[event.data.func].apply(null,event.data.args);
            } catch(e) {
                resultException(e);
                break;
            }
            if(event.data.async) {
                let aid = __nextAsyncID;
                __nextAsyncID += 1;

                postMessage({
                    type: 'resultpending',
                    aid: aid});

                Promise.resolve(r)
                    .then(r => {
                        postMessage({
                            type: 'asyncresult',
                            value: r,
                            aid: aid});
                    })
                    .catch(r => {
                        postMessage({
                            type: 'asyncresultexception',
                            message: r.toString(),
                            aid: aid});
                    });
            } else {
                postMessage({
                    type: 'result',
                    value: r});
            }
            break;
        case 'modulename':
            byName = __moduleCache[event.data.name];
            r = __moduleCacheById[event.data.mid];
            if(r === undefined) {
                __moduleCacheById[event.data.mid] = byName;
                postMessage({
                    type: 'import',
                    mid: event.data.mid});
            } else if(r instanceof __PendingMod) {
                r.mergeFrom(byName);
                for(let item of byName.aliases) __moduleCache[item] = r;
            } else {
                byName.resolve(r);
            }
            break;
        case 'module':
            onmodulemessage(event.data);
            break;
        case 'moduleerror':
            r = event.data.name === null ?
                __moduleCacheById[event.data.mid] :
                __moduleCache[event.data.name];
            console.assert(r instanceof __PendingMod);
            r.reject(new JailImportError(event.data.message));
            break;
        default:
            postMessage({
                type: 'error',
                message: 'unknown request type'})
        }
    } catch(e) {
        postMessage({
            type: 'error',
            message: e.message,
            filename: e.filename,
            lineno: e.lineNumber});
    }
};

function jimport(m) {
    return new Promise((resolve,reject) => {
        let r = __moduleCache[m];
        if(r instanceof __PendingMod) r.add(resolve,reject);
        else if(r === undefined) {
            __moduleCache[m] = new __PendingMod(m,resolve,reject);
            postMessage({
                type: 'normalizename',
                name: m});
        }
        else resolve(r);
    });
}

// signal that we are ready to receive requests
postMessage({type: 'success'});

