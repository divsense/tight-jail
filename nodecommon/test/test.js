const chai = require('chai');
chai.use(require('chai-as-promised'));
const assert = chai.assert;
const jail = require('../index.js');

function sleep(ms) {
    return new Promise(resolve => { setTimeout(resolve,ms); });
}

function moduleResolver(mid) {
    let m;
    switch(mid) {
    case 'a':
    case 'not_a':
    case 'w':
    case null:
        return 'exports.square = x => x * x;';
    case 'b':
        return Promise.resolve('exports.cube = x => x * x * x;');
    case 'c':
        /*
        (module
          (func (export "fourthPow") (param i32) (result i32)
            get_local 0
            get_local 0
            get_local 0
            get_local 0
            i32.mul
            i32.mul
            i32.mul
          )
        )
        */
        return Buffer.from([
            0,0x61,0x73,0x6d,0x01,0,0,0,0x01,0x06,0x01,0x60,0x01,0x7f,0x01,0x7f,
            0x03,0x02,0x01,0,0x07,0x0d,0x01,0x09,0x66,0x6f,0x75,0x72,0x74,0x68,
            0x50,0x6f,0x77,0,0,0x0a,0x0f,0x01,0x0d,0,0x20,0,0x20,0,0x20,0,0x20,
            0,0x6c,0x6c,0x6c,0x0b]);
    case 'd':
        return 'exports.square = UNDEFINED_VALUE;';
    case 'e':
        return 'exports.square BAD SYNTAX;';
    case 'f':
        return Buffer.from([0,1,2,3,4]);
    case 'z':
        throw new Error('something');
    case 'x':
        return Promise.resolve(null);
    case 'v':
        return new Promise((resolve,reject) => { reject(new Error('something else')); });
    default:
        m = /times([0-9]+)/.exec(mid);
        if(m) return 'exports.mult = x => x * ' + m[1]; + ';';
        return null;
    }
}

function countingModResolver(results) {
    return mid => {
        let c = results[mid];
        results[mid] = 1 + (c || 0);
        return moduleResolver(mid);
    };
}

function moduleNameNormalizer(name) {
    name = name.toLowerCase();
    switch(name) {
    case 'not_a':
        return null;
    case 'y':
        throw new Error('nothing');
    case 'w':
        return Promise.resolve(null);
    case 'u':
        return new Promise((resolve,reject) => { reject(new Error('nothing else')); });
    default:
        return name;
    }
}

function commonTests(C) {
    it('should be able to evaluate JavaScript',async function () {
        var result = await (new C(true)).eval('3 * 6');
        assert.equal(result,18);
    });

    it('should be closed automatically when autoClose is true',async function () {
        var j = new C();
        try {
            var ccount = (await j.getStats()).connections;
            var result = await (new C(true)).eval('2 + 2');
            assert.equal(result,4);
            while((await j.getStats()).connections != ccount) await sleep(10);
        } finally {
            j.close();
        }
    });

    it('should terminate execution upon closing a connection',async function() {
        var j1 = new C();
        try {
            var ccount1 = (await j1.getStats()).connections;
            var j2 = new C();
            try {
                await j2.eval('2 + 2'); // wait for j2 to be usable
                assert.notEqual((await j1.getStats()).connections,ccount1);

                var j2p = j2.exec('while(true);');
            } finally {
                j2.close();
            }
            await assert.isRejected(j2p,jail.ClosedJailError);
            while((await j1.getStats()).connections != ccount1) await sleep(10);
        } finally {
            j1.close();
        }
    });

    it('should be able to call functions',async function () {
        var result = await (new C(true)).call('eval',['5 + 6']);
        assert.equal(result,11);
    });

    /*it('should be able to execute remote files',async function () {
        var j = new C();
        try {
            await j.execURI('https://divsense.github.io/tight-jail/samples/basic.js');
            assert.equal(await j.call('test',[4,5,6]),54);
        } finally {
            j.close();
        }
    });*/
}

describe('tight-jail',function () {
    after(function() {
        jail.shutdown();
    });

    describe('JailConnection',function () {
        commonTests(jail.JailConnection);

        it('should not retain state without an explicit context',async function () {
            var j = new jail.JailConnection();
            try {
                await j.exec('var x = 6');
                await assert.isRejected(j.eval('x'),jail.ClientError);
            } finally {
                j.close();
            }
        });

        it('should retain state with an explicit context',async function () {
            var j = new jail.JailConnection();
            try {
                var c = await j.createContext();
                await j.exec('var x = 7',c);
                var result = await j.eval('x',c);
                assert.equal(result,7);
            } finally {
                j.close();
            }
        });

        it('should throw ContextJailError with invalid context ids',async function () {
            var j = new jail.JailConnection();
            try {
                await assert.isRejected(j.eval('2 + 2',1),jail.ContextJailError);
                await assert.isRejected(j.destroyContext(1),jail.ContextJailError);
                var c = await j.createContext();
                await assert.isRejected(j.eval('2 + 2',c+1),jail.ContextJailError);
                await assert.isRejected(j.destroyContext(c+1),jail.ContextJailError);
            } finally {
                j.close();
            }
        });
    });

    describe('JailContext',function () {
        commonTests(jail.JailContext);

        it('should retain state',async function () {
            var j = new jail.JailContext();
            try {
                await j.exec('var x = "hello"');
                var result = await j.eval('x');
                assert.equal(result,'hello');
            } finally {
                j.close();
            }
        });

        it('should be able to call async functions',async function () {
            var j = new jail.JailContext();
            try {
                j.exec('async function X(a) { return a*5; }' +
                    'async function Y(a) { return await X(a+2); }' +
                    'function Z() { return new Promise((resolve,reject) => { reject(new Error("yo")); }) }');

                assert.equal(await j.call('Y',[2],true),20);
                await assert.isRejected(j.call('Z',[],true),jail.ClientError);
            } finally {
                j.close();
            }
        });

        it('should be able to import JS modules',async function () {
            jail.purgeCache();
            jail.setModuleLoader(moduleResolver);
            var j = new jail.JailContext();
            try {
                j.exec('async function X(a) {' +
                           'var m = await jimport("a");' +
                           'return m.square(a); }' +
                       'async function Y(a) {' +
                           'var m = await jimport("b");' +
                           'return m.cube(a); }');
                assert.equal(await j.call('X',[3],true),9);
                assert.equal(await j.call('Y',[4],true),64);
            } finally {
                j.close();
            }
        });

        it('should be able to import WASM modules',async function () {
            jail.purgeCache();
            jail.setModuleLoader(moduleResolver);
            var j = new jail.JailContext();
            try {
                j.exec('async function X(a) {' +
                           'var m = await jimport("c");' +
                           'return m.fourthPow(a); }');
                assert.equal(await j.call('X',[7],true),2401);
            } finally {
                j.close();
            }
        });

        it('importing the same module should return the same instance',async function () {
            jail.purgeCache();
            jail.setModuleLoader(moduleResolver);
            var j = new jail.JailContext();
            try {
                await j.exec('async function X(mod1,mod2) {' +
                                 'var a = await jimport(mod1);' +
                                 'var b = await jimport(mod2);' +
                                 'return a === b; }')
                assert.isOk(await j.call('X',['a','a'],true));
                assert.isOk(await j.call('X',['b','b'],true));
                assert.isNotOk(await j.call('X',['a','b'],true));
            } finally {
                j.close();
            }
        });

        it('should be able to normalize module names',async function () {
            jail.purgeCache();
            jail.setModuleLoader(moduleResolver,moduleNameNormalizer);
            var j = new jail.JailContext();
            try {
                await j.exec('async function X(mod1,mod2) {' +
                                 'var a = await jimport(mod1);' +
                                 'var b = await jimport(mod2);' +
                                 'return a === b; }')
                assert.isOk(await j.call('X',['a','A'],true));
                assert.isOk(await j.call('X',['b','B'],true));
                assert.isNotOk(await j.call('X',['a','b'],true));
            } finally {
                j.close();
            }
        });

        it('cache should persist across contexts',async function () {
            jail.purgeCache();
            var counts = {};
            jail.setModuleLoader(countingModResolver(counts));
            const code = 'async function X(x) { return (await jimport("a")).square(x); }\n' +
                'async function Y(x) { return (await jimport("c")).fourthPow(x); }';

            var j = new jail.JailContext();
            try {
                j.exec(code)
                assert.equal(await j.call('X',[4],true),16);
                assert.equal(await j.call('Y',[4],true),256);
                assert.equal(counts['a'],1);
                assert.equal(counts['c'],1);
            } finally {
                j.close();
            }

            var j = new jail.JailContext();
            try {
                j.exec(code)
                assert.equal(await j.call('X',[6],true),36);
                assert.equal(await j.call('Y',[6],true),1296);
                assert.equal(counts['a'],1);
                assert.equal(counts['c'],1);
            } finally {
                j.close();
            }
        });

        it('should be able to clear the cache',async function () {
            jail.purgeCache();
            var counts = {};
            jail.setModuleLoader(countingModResolver(counts));
            const code = 'async function X(x) { return (await jimport("a")).square(x); }';

            var j = new jail.JailContext();
            try {
                assert.equal((await j.getStats()).cacheitems,0);
                j.exec(code)
                assert.equal(await j.call('X',[6],true),36);
                assert.equal(counts['a'],1);
                assert.equal(await j.call('X',[7],true),49);
                assert.equal(counts['a'],1);
                assert.equal((await j.getStats()).cacheitems,1);
            } finally {
                j.close();
            }

            jail.purgeCache();
            var j = new jail.JailContext();
            try {
                assert.equal((await j.getStats()).cacheitems,0);
                j.exec(code)
                assert.equal(await j.call('X',[8],true),64);
                assert.equal(counts['a'],2);
                assert.equal((await j.getStats()).cacheitems,1);
            } finally {
                j.close();
            }
        });

        it('should be able to remove individual items from the cache',async function () {
            jail.purgeCache();
            var counts = {};
            jail.setModuleLoader(countingModResolver(counts));
            const code = 'async function X(x) {' +
                'return (await jimport("a")).square(x) + (await jimport("b")).cube(x); }';

            var j = new jail.JailContext();
            try {
                assert.equal((await j.getStats()).cacheitems,0);
                j.exec(code)
                assert.equal(await j.call('X',[2],true),12);
                assert.equal(counts['a'],1);
                assert.equal(counts['b'],1);
                assert.equal((await j.getStats()).cacheitems,2);
            } finally {
                j.close();
            }

            jail.purgeCache(['a']);
            var j = new jail.JailContext();
            try {
                assert.equal((await j.getStats()).cacheitems,1);
                j.exec(code)
                assert.equal(await j.call('X',[3],true),36);
                assert.equal(counts['a'],2);
                assert.equal(counts['b'],1);
                assert.equal((await j.getStats()).cacheitems,2);
            } finally {
                j.close();
            }
        });

        it('errors thrown by the loader should be passed to the jailed code',async function () {
            jail.purgeCache();
            jail.setModuleLoader(moduleResolver,moduleNameNormalizer);
            var j = new jail.JailContext();
            try {
                j.exec('async function X(mod) { try { await jimport(mod); return false; } catch(e) { return true; } }');
                assert.isOk(await j.call('X',['z'],true));
                assert.isOk(await j.call('X',['y'],true));
                assert.isOk(await j.call('X',['v'],true));
                assert.isOk(await j.call('X',['u'],true));
                assert.isNotOk(await j.call('X',['a'],true));
            } finally {
                j.close();
            }
        });

        it('returning null in the loader should cause a module-not-found error',async function () {
            jail.purgeCache();
            jail.setModuleLoader(moduleResolver,moduleNameNormalizer);
            var j = new jail.JailContext();
            try {
                j.exec('async function X(mod) { try { await jimport(mod); return false; }' +
                    ' catch(e) { return e.toString() == "JailImportError: module not found"; } }');
                assert.isOk(await j.call('X',['not_b'],true));

                /* this one is important because moduleNameNormalizer('not_a')
                returns null but moduleResolver('not_a') and
                moduleResolver(null) does not */
                assert.isOk(await j.call('X',['not_a'],true));

                assert.isOk(await j.call('X',['x'],true));
                assert.isOk(await j.call('X',['w'],true));

                assert.isNotOk(await j.call('X',['a'],true));
            } finally {
                j.close();
            }
        });

        it('cache size should not exceed maximum',async function () {
            jail.purgeCache();
            jail.setModuleLoader(moduleResolver,null,5);

            let j = new jail.JailContext();
            try {
                j.exec('async function X(mod) { return (await jimport(mod)).mult(5); }')
                for(let i=0; i<10; ++i) {
                    assert.equal(await j.call('X',['times' + i],true),5 * i);
                    assert.equal((await j.getStats()).cacheitems,Math.min(i + 1,5));
                }
            } finally {
                j.close();
            }
        });

        it('should handle invalid modules gracefully',async function() {
            jail.purgeCache();
            jail.setModuleLoader(moduleResolver);
            var j = new jail.JailContext();
            try {
                j.exec('async function X(mod) { try { await jimport(mod); return false; }' +
                    ' catch(e) { return e instanceof JailImportError; } }');
                assert.isOk(await j.call('X',['d'],true));
                assert.isOk(await j.call('X',['e'],true));
                assert.isOk(await j.call('X',['f'],true));
                assert.isNotOk(await j.call('X',['a'],true));
            } finally {
                j.close();
            }
        });
    });
});
