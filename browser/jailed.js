'use strict';

onmessage = (event) => {
    /* calling eval indirectly will cause it to execute in global scope, which
       is what we want */
    const eval_ = eval;
    
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
        default:
            postMessage({
                type: 'error',
                errtype: 'request',
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

// signal that we are ready to receive requests
postMessage({type: 'success'});

