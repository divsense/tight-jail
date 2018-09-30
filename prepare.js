const fs = require('fs');
const path = require('path');

const rootDir = __dirname;

function escape_str(x) {
    return JSON.stringify(x.toString()).slice(1,-1);
}

function preprocess(input,defines) {
    let preproc_re = /\/\/\s*#\s*(\w+)(.*)(?:\r\n|\r|\n|$)/g;
    let code = ['let r = [];\n'];
    let lastI = 0;
    function add_part(part) {
        let lines = part.split(/\r\n|\r|\n/);
        let last = lines.pop();
        for(let line of lines) {
            code.push('r.push("');
            code.push(escape_str(line));
            code.push('\\n');
            code.push('");\n');
        }
        if(last) {
            code.push('r.push("');
            code.push(escape_str(line));
            code.push('");\n');
        }
    }

    let m;
    while(m = preproc_re.exec(input)) {
        if(m.index > lastI) add_part(input.slice(lastI,m.index));
        switch(m[1]) {
        case 'if':
            code.push(`if(${m[2].trim()}) {\n`);
            break;
        case 'elif':
            code.push(`} else if(${m[2].trim()}) {\n`);
            break;
        case 'else':
            if(m[2].trim()) throw new Error('"else" should not be followed by anything');
            code.push('} else {\n');
            break;
        case 'endif':
            if(m[2].trim()) throw new Error('"endif" should not be followed by anything');
            code.push('}\n');
            break;
        default:
            throw new Error(`invalid preprocessor command "${m[1]}"`);
        }
        lastI = preproc_re.lastIndex;
    }
    if(lastI < input.length) add_part(input.slice(lastI));

    code.push('return r.join("");\n');
    return (new Function(code.join(''))).apply(defines);
}

function processDir(srcdir,destdir,defines) {
    let paths = [srcdir];
    do {
        let dir = paths.shift();
        for(let name of fs.readdirSync(dir)) {
            if(name.startsWith('.')) continue;
            let full = path.join(dir,name);
            let entry = fs.lstatSync(full);

            if(entry.isDirectory()) paths.push(full);
            else if(entry.isFile() && name.endsWith('.js')) {
                fs.writeFileSync(
                    path.join(destdir,path.relative(srcdir,full)),
                    preprocess(fs.readFileSync(full,{encoding:'utf8'}),defines));
            }
        }
    } while(paths.length);
}

let destdir = null;
let posOnly = false;
let defines = {};

for(let i=2; i<process.argv.length; ++i) {
    let arg = process.argv[i];

    if(!posOnly && arg.startsWith('-')) {
        if(arg == '--') {
            posOnly = true;
        }
        else if(arg.startsWith('-D')) {
            let eqi = arg.indexOf('=',2);
            if(eqi == -1) defines[arg.slice(2)] = true;
            else defines[arg.slice(2,eqi)] = arg.slice(eqi+1);
        }
        else throw new Error(`invalid argument "${arg}"`);
    }
    else {
        if(destdir) throw new Error('only one destination dir allowed');
        destdir = arg;
    }
}

if(!destdir) throw new Error('destination dir not specified');

processDir(path.join(rootDir,'nodecommon'),destdir,defines);

