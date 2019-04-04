
class RequestError extends Error {
    constructor(msg,errtype='request') {
        super(msg);
        this.errtype = errtype;
    }
}
exports.RequestError = RequestError;

function reqAttr(msg,attr) {
    let r = msg[attr];
    if(r === undefined) throw new RequestError(`required attribute "${attr}" missing`);
    return r;
}
exports.reqAttr = reqAttr;

exports.reqString = function (msg,attr) {
    let r = reqAttr(msg,attr);
    if(typeof r != 'string') throw new RequestError(`attribute "${attr}" must be a string`);
    return r;
}

exports.reqInt = function (msg,attr) {
    let r = reqAttr(msg,attr);
    if(typeof r != 'number') throw new RequestError(`attribute "${attr}" must be a number`);
    return r;
}

exports.reqArray = function (msg,attr) {
    let r = reqAttr(msg,attr);
    if(!(r instanceof Array)) throw new RequestError(`attribute "${attr}" must be an array`);
    return r;
}

exports.reqBool = function (msg,attr) {
    let r = reqAttr(msg,attr);
    if(typeof r != 'boolean') throw new RequestError(`attribute "${attr}" must be a boolean`);
    return r;
}

exports.parseJSONForReq = function (json) {
    try {
        return JSON.parse(json);
    } catch(e) {
        throw new RequestError('invalid JSON');
    }
}

