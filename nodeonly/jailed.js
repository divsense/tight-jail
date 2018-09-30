const vm = require('vm');
const net = require('net');
const path = require('path');
const worker = require('worker_threads');


if(process.argv.length != 3) {
    throw new Error('exactly one argument is required');
}

class Conn {
    constructor(conn,worker) {
        this.c = conn;
        this.w = worker;
    }
    
    close() {
        this.c.destroy();
        this.w.terminate();
        this.c.removeAllListeners('error');
        this.c.removeAllListeners('close');
    }
}
Conn.connections = new Set();

let server = net.createServer((c) => {
    c.setEncoding('utf8');
    
    let conn = new Conn(
        c,
        new worker.Worker(path.join(__dirname,'jail_worker.js')));
    Conn.connections.add(conn);
    
    let buffer = '';
    c.on('data',(data) => {
        let results = data.split('\x00').reverse();
        while(results.length) {
            buffer += results.pop();
            if(results.length > 0) {
                conn.w.postMessage(buffer);
                buffer = '';
            }
        }
    });
    
    c.on('error',(e) => {
        console.error(e);
        conn.close();
        Conn.connections.delete(conn);
    });
    
    c.on('close',() => {
        conn.close();
        Conn.connections.delete(conn);
    });
    
    conn.w.on('message',(msg) => {
        c.write(JSON.stringify(msg) + '\x00');
    });
});

server.listen(process.argv[2],() => {
    process.stdout.write('ready\n');
});

process.on('SIGINT',() => {
    console.error('shutting down');
    server.close();
    Conn.connections.forEach(c => c.close());
});


