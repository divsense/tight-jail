const chai = require('chai');
chai.use(require('chai-as-promised'));
const assert = chai.assert;
const jail = require('../index.js');

describe('tight-jail',function () {
    after(function() {
        jail.shutdown();
    });
    
    describe('JailConnection',function () {
        it('should be able to evaluate JavaScript',async function () {
            var result = await (new jail.JailConnection(true)).eval('3 * 6');
            assert.equal(result,18);
        });

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
        it('should be able to evaluate JavaScript',async function () {
            var result = await (new jail.JailContext(true)).eval('3 * 6');
            assert.equal(result,18);
        });

        it('should retain state',async function () {
            var j = new jail.JailContext();
            try {
                await j.exec('var x = 7');
                var result = await j.eval('x');
                assert.equal(result,7);
            } finally {
                j.close();
            }
        });
    });
});
