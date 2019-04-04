const vm = require('vm');
const worker = require('worker_threads');
const {RequestError,reqString,reqInt,reqArray,reqBool,parseJSONForReq} = require('./requests.js');


const contexts = new Map();

let nextId = 1;
let nextAsyncId = 1;


function tryGetAttr(obj,attr) {
    try {
        return obj[attr];
    } catch(e) {
        return null;
    }
}

class CodeExc extends Error {
    constructor(msg,filename,lineno) {
        super(msg);
        this.userFilename = filename;
        this.userLineNo = lineno;
    }

    static fromError(e) {
        let exc_msg = null;
        try {
            exc_msg = e.toString();
        } catch(e2) {}

        return new CodeExc(
            exc_msg,
            tryGetAttr(e,'fileName'),
            tryGetAttr(e,'lineNumber'));
    }
}


class JailContext {
    constructor(context,resolveId,resolveMod,failMod) {
        this.context = context;
        this.resolveId = resolveId;
        this.resolveMod = resolveMod;
        this.failMod = failMod;
    }

    run(code) {
        try {
            return vm.runInContext(code,this.context);
        } catch(e) {
            throw CodeExc.fromError(e);
        }
    }
}

function newContext(makeWeak,withExports=false) {
    let id = nextId++;

    class JailImportError extends Error {}
    JailImportError.prototype.name = 'JailImportError';

    const moduleCacheByName = {};
    const moduleCacheById = {};

    class PendingMod {
      constructor(resolve1,reject1) {
        this.callbacks = [[resolve1,reject1]];
        this.aliases = [];
      }

      add(resolve,reject) {
        this.callbacks.push([resolve,reject]);
      }

      mergeFrom(b,name) {
        for(let c of b.callbacks) this.callbacks.push(c);
        for(let a of b.aliases) this.aliases.push(a);
        this.aliases.push(name);
      }

      resolve(x,name) {
        moduleCacheByName[name] = x;
        for(let a of this.aliases) moduleCacheByName[a] = x;
        for(let c of this.callbacks) c[0](x);
      }

      reject(x,name) {
        delete moduleCacheByName[name];
        for(let a of this.aliases) delete moduleCacheByName[a];
        for(let c of this.callbacks) c[1](x);
      }
    }

    function jimport(m) {
      return new Promise((resolve,reject) => {
        let r = moduleCacheByName[m];
        if(r === null) reject(new JailImportError('module not found'));
        else if(r instanceof PendingMod) r.add(resolve,reject);
        else if(r === undefined) {
          moduleCacheByName[m] = new PendingMod(resolve,reject);
          worker.parentPort.postMessage({
              type: 'import',
              context: id,
              name: m});
        }
        else resolve(r);
      });
    }

    let globals = {
        console: {log(x) {}},
        JailImportError,
        jimport
    };
    if(withExports) globals.exports = {};

    let r = new JailContext(
        vm.createContext(globals),
        function (name,mid) {
          let pending = moduleCacheByName[name];
          let r = moduleCacheById[mid];
          if(r === undefined || r === pending) {
            moduleCacheById[mid] = pending;
            return true;
          }

          if(r instanceof PendingMod) {
            r.mergeFrom(pending,name);
            moduleCacheByName[name] = r;
            for(let a of pending.aliases) moduleCacheByName[a] = r;
            return false;
          }

          pending.resolve(r,name);
          return false;
        },
        function (name,mid,mod) {
          let pending = moduleCacheByName[name];
          moduleCacheById[mid] = mod;
          pending.resolve(mod,name);
        },
        function (name,mid,message) {
          if(mid !== null) delete moduleCacheById[mid];
          moduleCacheByName[name].reject(new JailImportError(message.toString()),name);
        });
    contexts.set(id,r);

    return [id,r];
}

function getContext(id=null) {
    if(id) {
        let r = contexts.get(id);
        if(r === undefined) throw new RequestError('no such context','context');
        return [id,r];
    }
    return newContext(true);
}

function postResponse(msg) {
    worker.parentPort.postMessage({type: "response",value: JSON.stringify(msg)});
}

// this is for "eval" and, when "async" is false, "call"
function postResponseUserVal(msg) {
    try {
        postResponse(msg);
    } catch(e) {
        /* the filename and line number is not included since the exception was
        not raised in the user's code */
        let errstr = null;
        try {
            errstr = e.toString();
        } catch(e) {}
        postResponse({
            type: 'resultexception',
            message: errstr,
            filename: null,
            lineno: null});
    }
}

function handleRequest(msgstr) {
    try {
        let msg = parseJSONForReq(msgstr);
        switch(reqString(msg,'type')) {
        case 'eval':
            {
                let [cid,context] = getContext(msg.context);
                try {
                    postResponseUserVal({
                        type: 'result',
                        value: context.run(reqString(msg,'code'))});
                } finally {
                    if(!msg.context) contexts.delete(cid);
                }
            }
            break;
        case 'exec':
            {
                let [cid,context] = getContext(msg.context);
                try {
                    getContext(msg.context)[1].run(reqString(msg,'code'));
                    postResponse({type: 'success'});
                } finally {
                    if(!msg.context) contexts.delete(cid);
                }
            }
            break;
        case 'call':
            {
                let func = reqString(msg,'func');
                let runAsync = reqBool(msg,'async');
                let args = reqArray(msg,'args');
                if(!func.match(/[_a-zA-Z][_a-zA-Z0-9]*/))
                    throw new RequestError('invalid function name');
                let [cid,context] = getContext(msg.context);

                let r = context.run(func);
                if((typeof r) != "function") throw new RequestError(`"${func}" is not a function`);
                r = r.apply(null,args);
                if(runAsync) {
                    let aid = nextAsyncId++;
                    let p = Promise.resolve(r)
                        .then(r => {
                            rmsg = {
                                type: 'asyncresult',
                                id: aid,
                                value: r};
                            try {
                                postResponse(rmsg);
                            } catch(e) {
                                /* the filename and line number is not included
                                since the exception was not raised in the user's
                                code */
                                let errstr = null;
                                try {
                                    errstr = e.toString();
                                } catch(e) {}
                                postResponse({
                                    type: 'asyncresultexception',
                                    id: aid,
                                    message: errstr,
                                    filename: null,
                                    lineno: null});
                            }
                        })
                        .catch(e => {
                            e = CodeExc.fromError(e);
                            postResponse({
                                type: 'asyncresultexception',
                                id: aid,
                                message: e.message,
                                filename: e.userFilename,
                                lineno: e.userLineNo});
                        });
                    if(!msg.context) p.then(() => { contexts.delete(cid); });
                    postResponse({type: 'resultpending',id: aid});
                } else {
                    postResponseUserVal({
                        type: 'result',
                        value: r});
                }
            }
            break;
        case 'createcontext':
            postResponse({type: 'result', value: newContext(false)[0]});
            break;
        case 'destroycontext':
            {
                let id = reqInt(msg,'context');
                if(!contexts.delete(id)) throw new RequestError('no such context','context');
                postResponse({type: 'success'});
            }
            break;
        case 'getstats':
            worker.parentPort.postMessage({type: "getstats"});
            break;
        case 'setdbgflags':
            worker.parentPort.postMessage({
                type: 'setdbgflags',
                value: reqArray(msg,'value')
            });
            break;
        default:
            throw new RequestError('unknown request type');
        }
    } catch(e) {
        if(e instanceof CodeExc) {
            postResponse({
                type: 'resultexception',
                message: e.message,
                filename: e.userFilename,
                lineno: e.userLineNo});
        } else if(e instanceof RequestError) {
            postResponse({
                type: 'error',
                errtype: e.errtype,
                message: e.message})
        } else {
            postResponse({
                type: 'error',
                errtype: 'internal',
                message: e.message});
        }
    }
}

worker.parentPort.on('message',msg => {
    switch(msg.type) {
    case 'request':
        handleRequest(msg.value);
        break;
    case 'moduleid':
        {
            let context = contexts.get(msg.context);
            if(context) {
                if(context.resolveId(msg.name,msg.id)) {
                    worker.parentPort.postMessage({
                        type: 'getmodule',
                        context: msg.context,
                        id: msg.id,
                        name: msg.name});
                } else {
                    worker.parentPort.postMessage({
                        type: 'releasemodule',
                        context: msg.context,
                        id: msg.id});
                }
            }
        }
        break;
    case 'module':
        if(msg.value instanceof WebAssembly.Module) {
            WebAssembly.instantiate(msg.value)
                .then(inst => {
                    let context = contexts.get(msg.context);
                    if(context) context.resolveMod(msg.name,msg.id,inst.exports);
                })
                .catch(e => {
                    let context = contexts.get(msg.context);
                    if(context) context.failMod(msg.name,msg.id,e);
                });
        } else if(typeof msg.value == 'string') {
            let script;
            try {
                script = new vm.Script(msg.value);
            } catch(e) {
                let context = contexts.get(msg.context);
                if(context) context.failMod(msg.name,msg.id,e);
                worker.parentPort.postMessage({
                    type: 'failcompiled',
                    context: msg.context,
                    id: msg.id,
                    message: e.toString()});
                break;
            }
            worker.parentPort.postMessage({
                type: 'setcompiled',
                context: msg.context,
                id: msg.id,
                value: script.createCachedData()});
            let context = contexts.get(msg.context);
            if(context) {
                let mod_context = newContext(true,true)[1];
                try {
                    script.runInContext(mod_context.context);
                } catch(e) {
                    context.failMod(msg.name,msg.id,e);
                    break;
                }

                context.resolveMod(
                    msg.name,
                    msg.id,
                    vm.runInContext('exports',mod_context.context));
            }
        } else {
            let context = contexts.get(msg.context);
            if(context) {
                let mod_context = newContext(true,true)[1];
                try {
                    (new vm.Script(msg.value[0],{cachedData: msg.value[1]}))
                        .runInContext(mod_context.context);
                } catch(e) {
                    context.failMod(msg.name,msg.id,e);
                    break;
                }
                context.resolveMod(
                    msg.name,
                    msg.id,
                    vm.runInContext('exports',mod_context.context));
            }
        }
        break;
    case 'moduleerror':
        {
            let context = contexts.get(msg.context);
            if(context) context.failMod(msg.name,msg.id,msg.message);
        }
        break;
    default:
        console.assert(false);
    }
});
