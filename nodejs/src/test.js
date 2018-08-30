const {JSJail} = require('./jsjail.js');


var sandbox = new JSJail();
var sandbox2 = new JSJail();

(async function () {
    try {
        var context = await sandbox.createContext();
        await sandbox.exec("var x = 2 * 5;",context);
        var result = await sandbox.eval("x",context);
        console.log(result);
        await sandbox.exec("var x = 2 * 5;");
        try {
            var result = await sandbox.eval("x");
            console.log(result);
        } catch(e) {
            console.log(e.toString());
        }
        try {
            var result = await sandbox2.eval("x",context);
            console.log(result);
        } catch(e) {
            console.log(e.toString());
        }
    } catch(e) {
        console.log(e);
    }
    JSJail.shutdown();
})()

