const chai = require('chai');
chai.use(require('chai-as-promised'));
const assert = chai.assert;
const jail = require('../index.js');

describe('JSJail',function () {
    it('should be able to evaluate JavaScript',async function () {
        var j = new jail.JailConnection();
        var result = await (new jail.JailConnection()).eval('3 * 6');
        assert.equal(result,18);
    });

    it('should not retain state without an explicit context',async function () {
    	var j = new jail.JailConnection();
        await j.exec('var x = 6');
        assert.isRejected(j.eval('x'),jail.ClientError);
    });
    
    it('should retain state with an explicit context',async function () {
        var j = new jail.JailConnection();
        var c = await j.createContext();
        await j.exec('var x = 7',c);
        var result = await j.eval('x',c);
        assert.equal(result,7);
    });
});
