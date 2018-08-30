
function postUserMessage(data) {
    postMessage({
        type: "user",
        data: data});
}

onmessage = function (event) {
    switch(event.data.type) {
    case "args":
        if(event.data.isLink) importScripts(event.data.script);
        else eval(event.data.script);
        
        var retval = main.apply(null,event.data.args);
        postMessage(
            {type: "success",result: retval});
        
        break;
    case "user":
        // call "onUserMessage" if it's defined
        try {
            onUserMessage;
        } catch(e) {
            break;
        }
        onUserMessage(event.data.data);
        break;
    }
};

