const vm = require('vm');
const worker = require('worker_threads');

let contexts = {};

let nextId = 1;


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

class RequestError extends Error {
    constructor(msg,errtype='request') {
        super(msg);
        this.errtype = errtype;
    }
}

function reqAttr(msg,attr) {
    let r = msg[attr];
    if(r === undefined) throw new RequestError(`required attribute "${attr}" missing`);
    return r;
}

function reqString(msg,attr) {
    let r = reqAttr(msg,attr);
    if(typeof r != 'string') throw new RequestError(`attribute "${attr}" must be a string`);
    return r;
}

function reqInt(msg,attr) {
    let r = reqAttr(msg,attr);
    if(typeof r != 'number') throw new RequestError(`attribute "${attr}" must be a number`);
    return r;
}

function reqArray(msg,attr) {
    let r = reqAttr(msg,attr);
    if(!(r instanceof Array)) throw new RequestError(`attribute "${attr}" must be an array`);
    return r;
}

function getContext(id) {
    if(!id) return vm.createContext();
    let r = contexts[id];
    if(!r) throw new RequestError('no such context','context');
    return r;
}

function runInContext(code,id) {
    if(id) {
        let con = getContext(id);
        try {
            return vm.runInContext(code,con);
        } catch(e) {
            throw CodeExc.fromError(e);
        }
    } else {
        try {
            return vm.runInNewContext(code);
        } catch(e) {
            throw CodeExc.fromError(e);
        }
    }
}

function postResponse(msg) {
    worker.parentPort.postMessage(msg);
}

worker.parentPort.on('message',(msg_str) => {
    let msg = JSON.parse(msg_str);
    let rmsg, id;
    try {
        switch(reqString(msg,'type')) {
        case 'eval':
            rmsg = {
                type: 'result',
                value: runInContext(reqString(msg,'code'),msg.context)};
            if(msg.context) rmsg.context = msg.context;
            postResponse(rmsg);
            break;
        case 'exec':
            runInContext(reqString(msg,'code'),msg.context);
            rmsg = {type: 'success'};
            if(msg.context) rmsg.context = msg.context;
            postResponse(rmsg);
            break;
        case 'call':
            let func = reqString(msg,'func');
            if(!/[_a-zA-Z][_a-zA-Z0-9]*/.match(func))
                throw new RequestError('invalid function name');
            rmsg = {
                type: 'result',
                value: runInContext(
                    `${func}(${reqString(msg,'args').map(JSON.stringify).join()})`,
                    msg.context)};
            if(msg.context) rmsg.context = msg.context;
            postResponse(rmsg);
            break;
        case 'createcontext':
            id = nextId++;
            contexts[id] = vm.createContext();
            postResponse({type: 'success', context: id});
            break;
        case 'destroycontext':
            id = reqInt(msg,'context');
            if(!(id in contexts)) throw new RequestError('no such context','context');
            delete contexts[id];
            postResponse({type: 'success', context: id});
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
});
