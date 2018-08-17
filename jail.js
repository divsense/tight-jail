
var _idCount = 0;
var _jails = {};

function uniqueId() {
    /* a random number is tacked on to prevent anyone from relying on the ID to
    have any particular value */
    var r = _idCount + "." + Math.random();
    _idCount += 1;
    return r;
}

function createFrame() {
    var f = document.createElement("iframe");
    f.sandbox = "allow-scripts allow-same-origin";
    f.style.display = "none";
    return f;
}

function cleanJail(id) {
    var j = _jails[id];
    delete _jails[id];
    if(j._autoclean) j._frame.parentElement.removeChild(j._frame);
    delete j._frame;
}

function runXJailed(callback,isLink,script,options) {
    if(options === undefined) options = {};
    var create = options.frame === undefined;
    if(create) options.frame = createFrame();
    if(options.args === undefined) options.args = [];
    
    var id = uniqueId();
    var jail = {
        _id: id,
        _frame: options.frame,
        _finishCallback: callback,
        _isLink: isLink,
        _script: script,
        msgCallback: options.msgCallback,
        _args: options.args,
        _autoclean: create,
        cancel: function() {
            if(this._frame) {
                this._frame.setAttribute("src","about:blank");
                cleanJail(this._id);
            }
        },
        postMessage: function(data) {
            if(this._frame) this._frame.contentWindow.postMessage({type: "user",data: data},"*");
        }};
    _jails[id] = jail;
    
    options.frame.setAttribute("src","jailed.html#" + id);
    document.body.appendChild(options.frame);
    
    return jail;
}

function runLinkJailed(callback,source,options) {
    return runXJailed(callback,true,source,options);
}

function runCodeJailed(callback,code,options) {
    return runXJailed(callback,false,code,options);
}

window.addEventListener("message",function (event) {
    var id = event.data.id;
    var jail = _jails[id];
    
    /* this can happen if an iframe was removed before its code finished */
    if(jail === undefined) return;
    
    // call it without "this" set
    var finish_c = jail._finishCallback;
    var user_c = jail.msgCallback;
    
    switch(event.data.type) {
    case "getargs":
        jail._frame.contentWindow.postMessage(
            {
                type: "args",
                isLink: jail._isLink,
                script: jail._script,
                args: jail._args},
            "*");
        
        // release the references
        delete jail._script;
        delete jail._args;
        
        break;
    case "success":
        finish_c(true,event.data.result);
        cleanJail(id);
        break;
    case "error":
        finish_c(false,{name: event.data.name,message: event.data.message});
        cleanJail(id);
        break;
    case "user":
        if(user_c) user_c(event.data.data);
        break;
    }
},false);

