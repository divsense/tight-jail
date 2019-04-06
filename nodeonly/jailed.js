const vm = require('vm');
const net = require('net');
const path = require('path');
const worker = require('worker_threads');
const {RequestError,reqAttr,reqString,reqInt,parseJSONForReq} = require('./requests.js');


/* force compilation to never finish (this only affects JS compilation because
WASM compilation is not delegated to a worker) */
const DBG_FAIL_COMPILATION = 1;

if(process.argv.length != 3) {
    throw new Error('exactly one argument is required');
}

if(!(process.stdin instanceof net.Socket)) {
    throw new Error('standard-in cannot be a file');
}


class ModUncompiled {
    constructor(id) {
        this.id = id;
        this.waiting = [];
    }
}

class ModUncompiledJS extends ModUncompiled {
    constructor(id,data) {
        super(id);
        this.data = data;
        this.compiler = null; // the ID of the connection compiling the module
    }

    queryData() {
        return {
            type: 'UNCOMPILED_JS',
            waitingcount: this.waiting.length};
    }
}

class ModUncompiledWASM extends ModUncompiled {
    queryData() {
        return {
            type: 'UNCOMPILED_WASM',
            waitingcount: this.waiting.length};
    }
}

class ModJS {
    constructor(id,data) {
        this.id = id;
        this.data = data;
    }

    queryData() {
        return {
            type: 'JS',
            waitingcount: 0};
    }
}

class ModWASM {
    constructor(id,data) {
        this.id = id;
        this.data = data;
    }

    queryData() {
        return {
            type: 'WASM',
            waitingcount: 0};
    }
}

const moduleCache = new Map();

/* find any cache items where the thread responsible for compiling, has been
terminated */
function auditModuleCache() {
    for(let [key,item] of moduleCache) {
        if(item instanceof ModUncompiled && item.compiler !== null) {
            let cc = Conn.connections.get(item.compiler);
            if(cc) continue;

            item.compiler = null;
            for(let [conn_id,context_id,name] of item.waiting) {
                cc = Conn.connections.get(conn_id);
                if(cc) cc.postModule(context_id,name,key,item);
            }
            item.waiting.length = 0;
        }
    }
}

class Conn {
    constructor(conn,worker) {
        this.c = conn;
        this.w = worker;
        this.compiling = 0;
        this.tempModCache = new Map();

        /* special flags to allow unit tests to control behaviour */
        this.dbgFlags = 0;
    }

    close() {
        console.assert(this.compiling || !this.tempModCache.size);

        this.c.destroy();
        this.c.removeAllListeners('error');
        this.c.removeAllListeners('close');
        this.c.removeAllListeners('data');
        this.w.removeAllListeners('message');

        this.w.terminate();

        if(this.compiling) auditModuleCache();
    }

    postModuleError(context,name,id,message) {
        this.w.postMessage({
            type: 'moduleerror',
            context,
            name,
            id,
            message});
    }

    postModule(context,name,id,cmod) {
        let tempc = this.tempModCache.get(id);
        if(tempc === undefined) {
            tempc = [cmod,new Set()];
            this.tempModCache.set(id,tempc);
        }
        tempc[0] = cmod;
        tempc[1].add(context);
        this.w.postMessage({
            type: 'moduleid',
            context,
            name,
            id});
    }

    releaseCacheItem(id,context) {
        let [cmod,contexts] = this.tempModCache.get(id);
        contexts.delete(context);
        if(!contexts.size) this.tempModCache.delete(id);
        return cmod;
    }
}
Conn.connections = new Map();
Conn.next_id = 1;


function zeroDelimittedData(f) {
    let buffer = '';
    return data => {
        let results = data.split('\x00').reverse();
        while(results.length) {
            buffer += results.pop();
            if(results.length > 0) {
                f(buffer);
                buffer = '';
            }
        }
    }
}

let server = net.createServer(c => {
    c.setEncoding('utf8');

    let id = Conn.next_id++;
    let conn = new Conn(
        c,
        new worker.Worker(path.join(__dirname,'jail_worker.js')));
    Conn.connections.set(id,conn);

    c.on('data',zeroDelimittedData(data => {
        conn.w.postMessage({
            type: 'request',
            value: data});
    }));

    c.on('error',e => {
        Conn.connections.delete(id);
        conn.close();
    });

    c.on('close',() => {
        Conn.connections.delete(id);
        conn.close();
    });

    conn.w.on('message',msg => {
        switch(msg.type) {
        case 'response':
            c.write(msg.value + '\x00');
            break;
        case 'import':
            process.stdout.write(JSON.stringify({
                type: 'import',
                connection: id,
                context: msg.context,
                name: msg.name
            }) + '\x00');
            break;
        case 'releasemodule':
            // we don't need data from the cache
            {
                let [cmod,contexts] = conn.tempModCache.get(msg.id);
                contexts.delete(msg.context);
                if(!contexts.size) conn.tempModCache.delete(msg.id);
            }
            break;
        case 'getmodule':
            // get the module data from the cache
            {
                let [cmod,contexts] = conn.tempModCache.get(msg.id);
                if(cmod instanceof ModUncompiled) {
                    if(cmod instanceof ModUncompiledJS && cmod.compiler === null) {
                        cmod.compiler = id;
                        conn.compiling += 1;

                        if(conn.dbgFlags & DBG_FAIL_COMPILATION) break;
                    } else {
                        cmod.waiting.push([id,msg.context,msg.name]);
                        break;
                    }
                } else {
                    contexts.delete(msg.context);
                    if(!contexts.size) conn.tempModCache.delete(msg.id);
                }
                conn.w.postMessage({
                    type: 'module',
                    id: msg.id,
                    name: msg.name,
                    context: msg.context,
                    value: cmod.data});
            }
            break;
        case 'setcompiled':
            {
                conn.compiling -= 1;
                let [cmod,contexts] = conn.tempModCache.get(msg.id);
                let newCmod = new ModJS(msg.id,[cmod.data,msg.value]);
                contexts.delete(msg.context);
                if(!contexts.size) conn.tempModCache.delete(msg.id);
                else conn.tempModCache.set(msg.id,[newCmod,contexts]);

                if(moduleCache.has(msg.id)) moduleCache.set(msg.id,newCmod);

                for(let [conn_id,context_id,name] of cmod.waiting) {
                    cc = Conn.connections.get(conn_id);
                    if(cc) cc.postModule(context_id,name,msg.id,newCmod);
                }
            }
            break;
        case 'failcompiled':
            {
                conn.compiling -= 1;
                let [cmod,contexts] = conn.tempModCache.get(msg.id);
                contexts.delete(msg.context);
                if(!contexts.size) conn.tempModCache.delete(msg.id);
                cmod.compiler = null;

                for(let [conn_id,context_id,name] of cmod.waiting) {
                    cc = Conn.connections.get(conn_id);
                    if(cc) cc.postModuleError(context_id,name,msg.id,msg.message);
                }
            }
            break;
        case 'getstats':
            c.write(JSON.stringify({
                type: 'result',
                value: {
                    connections: Conn.connections.size,
                    cacheitems: moduleCache.size,
                    compiling: conn.compiling
                }}) + '\x00');
            break;
        case 'setdbgflags':
            conn.dbgFlags = 0;
            for(let f of msg.value) {
                switch(f) {
                case 'FAIL_COMPILATION':
                    conn.dbgFlags |= DBG_FAIL_COMPILATION;
                    break;
                }
            }
            c.write('{"type":"success"}\x00');
            break;
        default:
            console.assert(false);
        }
    });
});

process.stdin.setEncoding('utf8');
process.stdin.on('data',zeroDelimittedData(datastr => {
    try {
        let data = parseJSONForReq(datastr);
        switch(data.type) {
        case 'modulecached':
        case 'module':
        case 'moduleerror':
            break;
        case 'purgecache':
            if(data.ids === null) moduleCache.clear();
            else for(let id of data.ids) moduleCache.delete(id);
            return;
        case 'querycache':
            {
                let value = {};
                for(let item of moduleCache) value[item[0]] = item[1].queryData();
                process.stdout.write(JSON.stringify({
                    type: 'result',
                    value
                }) + '\x00');
            }
            return;
        default:
            throw new RequestError('unknown request type');
        }

        let id, cmod, type, value;

        let conn_id = reqInt(data,'connection');
        let context_id = reqInt(data,'context');
        let name = reqString(data,'name');

        let cc = Conn.connections.get(conn_id);

        switch(data.type) {
        case 'modulecached':
            id = reqString(data,'id');
            cmod = moduleCache.get(id);
            if(cmod instanceof ModUncompiledWASM) {
                cmod.waiting.push([conn_id,context_id,name]);
            } else if(cc) {
                cc.postModule(context_id,name,id,cmod);
            }
            break;
        case 'module':
            id = reqString(data,'id');
            type = reqString(data,'modtype');
            value = reqString(data,'value');
            if(type != 'wasm' && type != 'js')
                throw RequestError('invalid module type');

            if(type == 'wasm') {
                cmod = new ModUncompiledWASM(id)
                moduleCache.set(id,cmod);

                try {
                    value = Buffer.from(value,'base64');
                } catch(e) {
                    throw new RequestError('invalid base64 data');
                }
                WebAssembly.compile(value)
                    .then(m => {
                        let newCmod = new ModWASM(id,m);
                        if(moduleCache.has(id)) moduleCache.set(id,newCmod);
                        let cc = Conn.connections.get(conn_id);
                        if(cc) cc.postModule(context_id,name,id,newCmod);
                        for(let [conn_id,context_id,name] of cmod.waiting) {
                            cc = Conn.connections.get(conn_id);
                            if(cc) cc.postModule(context_id,name,id,newCmod);
                        }
                        cmod.waiting.length = 0;
                    })
                    .catch(e => {
                        let msg = '<cannot convert to string>';
                        try {
                            msg = e.toString();
                        } catch(e2) {}
                        let cc = Conn.connections.get(conn_id);
                        if(cc) cc.postModuleError(context_id,name,id,msg);
                        for(let [conn_id,context_id,name] of cmod.waiting) {
                            cc = Conn.connections.get(conn_id);
                            if(cc) cc.postModuleError(context_id,name,id,msg);
                        }
                        cmod.waiting.length = 0;
                    });
            } else {
                cmod = new ModUncompiledJS(id,value)
                moduleCache.set(id,cmod);
                if(cc) cc.postModule(context_id,name,id,cmod);
            }
            break;
        case 'moduleerror':
            if(cc) cc.postModuleError(
                context_id,
                name,
                null,
                reqString(data,'msg'));
            break;
        default:
            console.assert(false);
        }
    } catch(e) {
        if(e instanceof RequestError) {
            process.stdout.write(JSON.stringify({
                type: 'error',
                errtype: e.errtype,
                message: e.message}) + '\x00')
        } else {
            process.stdout.write(JSON.stringify({
                type: 'error',
                errtype: 'internal',
                message: e.toString()}) + '\x00');
        }
    }
}));

server.listen(process.argv[2],() => {
    process.stdout.write('{"type":"ready"}\x00');
});

process.on('SIGINT',() => {
    console.error('shutting down');
    server.close();
    process.stdin.destroy();
    let conns = Conn.connections;
    Conn.connections = new Map();
    for(let [id,c] of conns) c.close();
});


