#!/usr/bin/env node
'use strict';

const os = require('os');
const fs = require('fs-extra');
const path = require('path');
const byline = require('byline');
const process = require('process');
const child_process = require('child_process');

const CONFIG_LOG_NAME = 'cfg_test_log.txt';
const OUTPUTNAMEBASE = 'jsjaild';
const SOURCE_DEST_FOLDER = 'daemon';
const SOURCE = 'main.cpp';
const TOOLSETS = ['clang','gcc','msvc'];

// v8 builds its own libc++
const v8_libs = ['v8','v8_libbase','v8_libplatform','icuuc','icui18n','c++'];
const v8_ninja_names = ['v8','v8_libbase','v8_libplatform','icuuc','icui18n','libc++'];

let v8_ninja_lookup = {};
for(let i=0; i<v8_ninja_names.length; ++i)
    v8_ninja_lookup[v8_ninja_names[i]] = v8_libs[i];

const v8_aux_files = ['icudtl.dat','natives_blob.bin','snapshot_blob.bin'];

const gcc_config = {
    CXXFLAGS: '-std=c++0x -O2 -Wall -pthread -fstack-protector-strong',
    CPPFLAGS: '-DNDEBUG',
    LDFLAGS: '-pthread -s -Wl,-z,now -Wl,-z,relro',
    LDLIBS: '-luv',
    NAME_FLAG: '-o',
    PREPROCESS_FLAG: '-E -P',
    INCLUDEPATH_FLAG: '-I',
    LIBPATH_FLAG: '-L',
    LINCLUDE_DOT: '-Wl,-rpath=.',
    make_lib(x) { return '-l'+x; }};

const msvc_config = {
    CXXFLAGS: '/Ox /EHc /GL /GS /volatile:iso /Wall',
    CPPFLAGS: '/DNDEBUG',
    LDFLAGS: '/MT',
    LDLIBS: 'uv.lib v8.lib v8_libbase.lib v8_libplatform.lib icuuc.lib icui18n.lib',
    NAME_FLAG: '/Fe',
    PREPROCESS_FLAG: '/EP',
    INCLUDEPATH_FLAG: '/I',
    LINCLUDE_DOT: '' /* the current directory is always searched on Windows */,
    make_lib(x) { return x+'.lib'; }};

const ov = {overwrite: true};


class BuildError extends Error {}
BuildError.prototype.name = 'BuildError';
class ChildError extends BuildError {}
ChildError.prototype.name = 'ChildError';

function execShell(command,options,resolve,reject,retFunc = null) {
    var shell, args;
    if(os.platform == 'win32') {
        shell = process.env.COMSPEC || 'cmd.exe';
        args = ['/c',command];
    } else {
        shell = '/bin/sh';
        args = ['-c',command];
    }
    var r = child_process.spawn(shell,args,options);
    
    var onExit = (code,signal) => {
        if(code !== null) {
            if(code == 0) resolve(retFunc && retFunc());
            else reject(new ChildError('child process returned non-zero value'));
        } else reject(new ChildError('child process was terminated'));
    };
    r.on('exit',onExit);
    
    r.on('error',(e) => {
        sh.removeListener('exit',onExit);
        reject(e);
    });
    
    return r;
}

function verifyExec(command,options) {
    return new Promise((resolve,reject) => {
        var sh = execShell(command,options,resolve,reject);
    });
}

class ConfigEnv {
    constructor() {
        this.testNo = 0;
        this.oldDir = process.cwd();
        this.log_f = null;
        try {
            this.log_f = fs.openSync(CONFIG_LOG_NAME,'w');
        } catch(e) {
            console.log(`unable to create log file ${CONFIG_LOG_NAME}`);
        }    
        try {
            this.tempDir = fs.mkdtempSync(path.join(os.tmpdir(),'config'));
        } catch(e) {
            if(this.log_f) fs.closeSync(this.log_f);
            throw e;
        }
        try {
            process.chdir(this.tempDir);
        } catch(e) {
            fs.closeSync(this.log_f);
            fs.rmdirSync(this.tempDir);
            throw e;
        }
    }
    
    relPath(x) {
        return path.join(this.oldDir,x);
    }
    
    nextTestNo() {
        return this.testNo++;
    }
    
    close() {
        process.chdir(this.oldDir);
        if(this.log_f) fs.closeSync(this.log_f);
        fs.removeSync(this.tempDir);
    }
    
    newTestWithInputFile(input,postfix = '') {
        var name = path.join(this.tempDir,'_' + this.nextTestNo() + postfix);
        var fd = fs.openSync(name,'w');
        fs.writeSync(fd,input);
        fs.closeSync(fd);
        return name;
    }
    
    logCommand(command,input) {
        if(this.log_f) {
            fs.writeSync(this.log_f,`Attempting to run:\n${command}\nwith input:\n${input}\n`);
        }
    }
    
    getPreprocessedLine(cfg,input) {
        return new Promise((resolve,reject) => {
            var input_fname = this.newTestWithInputFile(input,'.h');
            
            var command = `${cfg.CXX} ${cfg.PREPROCESS_FLAG} ${cfg.CPPFLAGS} ${input_fname}`;
            
            this.logCommand(command,input);
            
            var lastLine = '';
            var sh = execShell(
                command,
                {stdio: ['ignore','pipe',this.log_f]},
                resolve,
                reject,
                () => lastLine);
            
            sh.stdout.setEncoding('utf8');
            byline.createStream(sh.stdout).on('data',(line) => { lastLine = line; });
        });
    }
    
    verifyCompile(cfg,input) {
        return new Promise((resolve,reject) => {
            var input_fname = this.newTestWithInputFile(input,'.cpp');
            
            var command = `${cfg.CXX} ${cfg.CPPFLAGS} ${cfg.CXXFLAGS} ${cfg.LDFLAGS} ${input_fname}`;
            
            this.logCommand(command,input);
            
            execShell(
                command,
                {stdio: ['ignore','ignore',this.log_f]},
                resolve,
                reject);
        });
    }
}

async function failMessage(action,msg) {
    try {
        return await action;
    } catch(e) {
        if(e instanceof BuildError) throw new BuildError(msg);
        throw e;
    }
}

function stripSuffix(x,suf) {
    return x.endsWith(suf) ? x.slice(0,-suf.length) : x;
}

function readNinja(config) {
    if(config.V8_BUILD_DIR) {
        return new Promise((resolve,reject) => {
            let buildn = byline.createStream(
                fs.createReadStream(path.join(config.V8_BUILD_DIR,'build.ninja')));
            let root = null;
            let lpaths = new Set();
            let libs = [];
            let neededFiles = [];
            let uses_so = false;
            buildn.on('data',(line) => {
                if(!root) {
                    let m = /^ *command *=(?:.* |)--root=([^ ]+).*$/.exec(line);
                    if(m) {
                        root = path.join(config.V8_BUILD_DIR,m[1]);
                        return;
                    }
                }
                let m = /^build +([^$:]+) *: *phony +(.*)$/.exec(line);
                if(m) {
                    let lib = v8_ninja_lookup[m[1]];
                    if(lib) {
                        let libpath = path.join(config.V8_BUILD_DIR,stripSuffix(m[2],'.TOC'));
                        if(libpath.endsWith('.a') || libpath.endsWith('.lib')) {
                            libs.push(libpath);
                        } else if(libpath.endsWith('.so')) {
                            uses_so = true;
                            let base = path.basename(libpath,'.so');
                            if(base.startsWith('lib')) {
                                lpaths.add(path.dirname(libpath));
                                libs.push('-l'+base.slice(3));
                                neededFiles.push(libpath);
                            }
                        } else if(libpath.endsWith('.dll')) {
                            neededFiles.push(libpath);
                        }
                    }
                }
            });
            buildn.on('end',() => {
                for(let lib of libs) config.LDLIBS += ' '+lib;
                for(let p of lpaths) config.LDFLAGS += ` ${config.LIBPATH_FLAG}${p}`;
                if(root) config.CPPFLAGS +=
                    ` ${config.INCLUDEPATH_FLAG}${path.join(root,'include')}`;
                if(uses_so) config.LDFLAGS += ' ' + config.LINCLUDE_DOT;
                
                for(let af of v8_aux_files) {
                    let full = path.join(config.V8_BUILD_DIR,af);
                    if(fs.existsSync(full)) neededFiles.push(full);
                }
                resolve(neededFiles);
            });
            buildn.on('error',reject);
        });
    } else {
        for(let lib in v8_libs) config.LDLIBS += ' '+config.make_lib(lib);
        return Promise.resolve([]);
    }
}

var tools = process.env.TOOLS;
if(!(tools && tools in TOOLSETS)) {
    tools = os.platform == 'win32' ? 'msvc' : 'gcc';
}

var config = {RUN_CHECKS:true};
var nameFlag;
switch(tools) {
case 'clang':
case 'gcc':
    Object.assign(config,gcc_config);
    config.CXX = tools == 'clang' ? 'clang++' : 'g++';
    break;
case 'msvc':
    Object.assign(config,msvc_config);
    config.CXX = 'cl.exe';
    break;
}

for(let prop of ['CXXFLAGS','CPPFLAGS','LDFLAGS','LDLIBS']) {
    let env_val = process.env[prop];
    if(env_val) config[prop] += ' ' + env_val;
    env_val = process.env['npm_config_'+prop];
    if(env_val) config[prop] += ' ' + env_val;
}

for(let prop of ['CXX','NAME_FLAG','PREPROCESS_FLAG','RUN_CHECKS','V8_BUILD_DIR']) {
    let env_val = process.env['npm_config_'+prop] || process.env[prop];
    if(env_val) config[prop] = env_val;
}


(async function() {
    try {
        let neededFiles = await readNinja(config);

        var cenv = new ConfigEnv();
        try {
            if(config.RUN_CHECKS) {
                await failMessage(
                    cenv.verifyCompile(config,'int main() { return 0; }'),
                    'cannot compile with current settings')
    
                let ver = JSON.parse(await failMessage(
                    cenv.getPreprocessedLine(config,'#include <v8-version.h>\n[V8_MAJOR_VERSION,V8_MINOR_VERSION]'),
                    'V8 header files not found'));
    
                if(ver[0] < 6 || (ver[0] == 6 && ver[1] < 8))
                    throw new BuildError('V8 library too old; need at least version 6.8.0');
                
                await failMessage(
                    cenv.getPreprocessedLine(config,'#include <uv.h>\n'),
                    'libuv header files not found');
            }
            
            let dest = OUTPUTNAMEBASE;
            if(os.platform == 'win32') dest += '.exe';
            let src = cenv.relPath(path.join(SOURCE_DEST_FOLDER,SOURCE));
            let command = `${config.CXX} ${config.NAME_FLAG} ${dest} ${config.CPPFLAGS} ${config.CXXFLAGS} ${config.LDFLAGS} ${config.LDLIBS} ${src}`;
            console.log(command);
            await verifyExec(command,{stdio: ['ignore','inherit','inherit']});
            await fs.move(dest,cenv.relPath(path.join(SOURCE_DEST_FOLDER,dest)),ov);
        } finally {
            cenv.close();
        }
        for(let f of neededFiles)
            await fs.copyFile(f,path.join(SOURCE_DEST_FOLDER,path.basename(f)),ov);
    } catch(e) {
        if(e instanceof BuildError) console.log(e.message);
        else console.log(e);
        process.exitCode = 1;
    }
})();
