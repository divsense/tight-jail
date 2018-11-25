'use strict';

class __PendingMod {
    constructor(resolve1,reject1) {
        this.callbacks = [[resolve1,reject1]];
    }

    add(resolve,reject) {
        this.callbacks.push([resolve,reject]);
    }

    resolve(x) {
        for(let c of this.callbacks) c[0](x);
    }

    reject(x) {
        for(let c of this.callbacks) c[1](x);
    }
}

class JailImportError extends Error {}
JailImportError.prototype.name = 'JailImportError';

var __moduleCache = {};

onmessage = (event) => {
    /* calling eval indirectly will cause it to execute in global scope, which
       is what we want */
    const eval_ = eval;

    async function onmodulemessage(data) {
        let p = __moduleCache[data.name];
        try {
            console.assert(p instanceof __PendingMod);
            let v;
            if(data.value instanceof WebAssembly.Module) {
                v = await WebAssembly.instantiate(data.value);
            } else {
                throw new Error('TODO: implement');
            }
            __moduleCache[data.name] = v;
            p.accept(v);
        } catch(e) {
            delete __moduleCache[data.name];
            p.reject(e);
        }
    }

    try {
        switch(event.data.type) {
        case 'eval':
            postMessage({type: 'result',value: eval_(event.data.code)});
            break;
        case 'exec':
            eval_(event.data.code);
            postMessage({type: 'success'});
            break;
        case 'execuri':
            importScripts(event.data.uri);
            postMessage({type: 'success'});
            break;
        case 'call':
            postMessage({
                type: 'result',
                value: self[event.data.func].apply(null,event.data.args)});
            break;
        case 'module':
            onmodulemessage(event.data);
            break;
        case 'moduleerror':
            {
                let p = __moduleCache[event.data.name];
                console.assert(p instanceof __PendingMod);
                delete __moduleCache[event.data.name];
                p.reject(JailImportError(event.data.message));
            }
            break;
        default:
            postMessage({
                type: 'error',
                message: 'unknown request type'})
        }
    } catch(e) {
        postMessage({
            type: 'resultexception',
            message: e.message,
            filename: e.filename,
            lineno: e.lineno},'*');
    }
};

function jimport(m) {
    return new Promise((resolve,reject) => {
        let r = __moduleCache[m];
        if(r === null) reject(new JailImportError('module not found'));
        else if(r instanceof __PendingMod) r.add([resolve,reject]);
        else if(r === undefined) {
            __moduleCache[m] = new __PendingMod(resolve,reject);
            postMessage({
                type: 'import',
                name: m},'*');
        }
        else resolve(r);
    });

    return r;
}

// signal that we are ready to receive requests
postMessage({type: 'success'});

