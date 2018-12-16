#!/usr/bin/env node
'use strict';

const os = require('os');
const fs = require('fs-extra');
const path = require('path');
const byline = require('byline');
const child_process = require('child_process');

const CONFIG_LOG_NAME = 'cfg_test_log.txt';
const OUTPUTNAMEBASE = 'jsjaild';
const SOURCE_DEST_FOLDER = 'daemon';
const SOURCES = ['main.cpp','almost_json_parser.cpp'];
const TOOLSETS = ['clang','gcc','msvc','none'];

// v8 builds its own libc++
const v8_libs = ['v8','v8_libbase','v8_libplatform','icui18n','icuuc','c++'];
const v8_ninja_names = ['v8','v8_libbase','v8_libplatform','icui18n','icuuc','libc++'];
const v8_libcpp_dirs = [
    path.join('obj','buildtools','third_party','libc++','libc++'),
    path.join('obj','buildtools','third_party','libc++abi','libc++abi')];
const v8_monolithic = 'v8_monolith';
const libv8_i = v8_libs.indexOf('v8');
const libcpp_i = v8_libs.indexOf('c++');

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
    CXXFLAGS: '/nologo /Ox /EHsc /GL /GS /volatile:iso /W4 /wd4100 /wd4706',
    CPPFLAGS: '/DNDEBUG',
    LDFLAGS: '/MT /link',
    LDLIBS: 'libuv.lib Ws2_32.lib Advapi32.lib User32.lib Psapi.lib Iphlpapi.lib Userenv.lib',
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

        /* Node's argument escaping seems to mess up our escaping, hence the
        need the extra quotes and windowsVerbatimArguments */
        args = ['/D','/C','"'+command+'"'];
        options = Object.assign({windowsVerbatimArguments: true},options || {});
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

            var command = `${cfg.CXX} ${cfg.CPPFLAGS} ${cfg.CXXFLAGS} ${input_fname} ${cfg.LDFLAGS}`;

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

function coalesce_str(...parts) {
    return parts.filter(x => !!x).join(' ');
}

function coalesce_str_into(config,attr,...parts) {
    config[attr] = coalesce_str(config[attr],...parts);
}

function str_to_bool(name,x) {
    x = x.toLowerCase();
    if(x == 'true' || x == '1') return true;
    if(x == 'false' || x == '0') return false;
    throw new BuildError(`${name} must be one of: true, false, 1 or 0`);
}

function files_in_folder(config,dir,suffix) {
    return fs.readdirSync(dir)
        .filter(name => name.endsWith(suffix))
        .map(name => config.shell_quote(path.join(dir,name)));
}

function readNinja(config) {
    if(config.V8_BUILD_DIR) {
        return new Promise((resolve,reject) => {
            let buildn = byline.createStream(
                fs.createReadStream(path.join(config.V8_BUILD_DIR,'build.ninja')));
            let root = null;
            let lpaths = new Set();

            // these need to be in the same order
            let libs = v8_libs.map(x => null);

            let neededFiles = [];
            let uses_so = false;
            let uses_a = false;

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
                    let i;
                    if(m[1] == v8_monolithic) i = libv8_i;
                    else i = v8_ninja_names.indexOf(m[1]);
                    if(i != -1) {
                        let libpath = path.join(config.V8_BUILD_DIR,stripSuffix(m[2],'.TOC'));
                        if(libpath.endsWith('.a')) {
                            libs[i] = libpath;
                        } else if(libpath.endsWith('.lib')) {
                            libs[i] = libpath;
                            let base = libpath.slice(0,-4);
                            if(base.endsWith('.dll')) neededFiles.push(base);
                        } else if(libpath.endsWith('.so')) {
                            uses_so = true;
                            let base = path.basename(libpath,'.so');
                            if(base.startsWith('lib')) {
                                lpaths.add(path.dirname(libpath));
                                libs[i] = '-l' + base.slice(3);
                                neededFiles.push(libpath);
                            }
                        }
                    }
                }
            });
            buildn.on('end',() => {
                if(root) coalesce_str_into(config,'CPPFLAGS',
                    config.INCLUDEPATH_FLAG + path.join(root,'include'));

                coalesce_str_into(config,'LDLIBS',...libs.filter(x => x).map(config.shell_quote));
                coalesce_str_into(
                    config,
                    'LDFLAGS',
                    ...Array.from(lpaths,p => config.shell_quote(config.LIBPATH_FLAG + p)));

                if(uses_so) coalesce_str_into(config,'LDFLAGS',config.LINCLUDE_DOT);

                if(!libs[libcpp_i]) {
                    for(let dir of v8_libcpp_dirs) {
                        let full = path.join(config.V8_BUILD_DIR,dir);
                        if(fs.existsSync(full))
                            coalesce_str_into(
                                config,
                                'LDLIBS',
                                ...files_in_folder(config,path.join(config.V8_BUILD_DIR,dir),'.o'));
                    }
                }

                for(let af of v8_aux_files) {
                    let full = path.join(config.V8_BUILD_DIR,af);
                    if(fs.existsSync(full)) neededFiles.push(full);
                }
                resolve(neededFiles);
            });
            buildn.on('error',reject);
        });
    } else {
        coalesce_str_into(config,'LDLIBS',...v8_libs.map(config.make_lib));
        return Promise.resolve([]);
    }
}


(async function() {
    try {
        var config = {
            RUN_CHECKS: true,
            CXXFLAGS: '',
            CPPFLAGS: '',
            LDFLAGS: '',
            LDLIBS: '',
            NAME_FLAG: '',
            PREPROCESS_FLAG: '',
            INCLUDEPATH_FLAG: '',
            LIBPATH_FLAG: '',
            LINCLUDE_DOT: '',
            make_lib(x) { return x; },
            shell_quote(x) { return `"${x}"`; }
        };

        if(os.platform == 'win32') config.shell_quote =
            x => '^"' + x.replace(/["^%|<>&]/,m => m == '"' ? '""' : '^'+m) + '^"';
        else config.shell_quote =
            x => "'" + x.replace("'","\\'") + "'";

        config.tools = process.env.npm_config_TOOLS || process.env.TOOLS;
        if(!(config.tools && TOOLSETS.includes(config.tools))) {
            config.tools = os.platform == 'win32' ? 'msvc' : 'gcc';
        }

        switch(config.tools) {
        case 'clang':
        case 'gcc':
            Object.assign(config,gcc_config);
            config.CXX = config.tools == 'clang' ? 'clang++' : 'g++';
            break;
        case 'msvc':
            Object.assign(config,msvc_config);
            config.CXX = 'cl.exe';
            break;
        case 'none':
            break;
        }

        for(let prop of ['CXXFLAGS','CPPFLAGS','LDFLAGS']) {
            coalesce_str_into(config,prop,process.env[prop],process.env['npm_config_'+prop]);
        }

        for(let prop of ['CXX','LDLIBS','NAME_FLAG','RUN_CHECKS','V8_BUILD_DIR']) {
            let env_val = process.env['npm_config_'+prop] || process.env[prop];
            if(env_val) config[prop] = env_val;
        }

        if(config.tools == 'none') config.RUN_CHECKS = false;
        else {
            let env_val = process.env.npm_config_RUN_CHECKS || process.env.RUN_CHECKS;
            if(env_val) config.RUN_CHECKS = str_to_bool('RUN_CHECKS',env_val);
        }

        if(config.CXX.trim() == '') throw new BuildError('CXX is not set');

        let neededFiles = [];
        if(config.tools != 'none') neededFiles = await readNinja(config);

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
            let src = SOURCES.map(x => cenv.relPath(path.join(SOURCE_DEST_FOLDER,x))).join(' ');
            let command = `${config.CXX} ${config.NAME_FLAG}${dest} ${config.CPPFLAGS} ${src} ${config.CXXFLAGS} ${config.LDLIBS} ${config.LDFLAGS}`;
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
