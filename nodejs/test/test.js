const chai = require('chai');
chai.use(require('chai-as-promised'));
const assert = chai.assert;
const jail = require('../index.js');

function sleep(ms) {
    return new Promise(resolve => { setTimeout(resolve,ms); });
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
            while((await j.getStats()).connections != ccount) sleep(10);
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
            while((await j1.getStats()).connections != ccount1) sleep(10);
        } finally {
            j1.close();
        }
    });
    
    it('should be able to call functions',async function () {
        var result = await (new C(true)).call('eval',['5 + 6']);
        assert.equal(result,11);
    });
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
    });
});
