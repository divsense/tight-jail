# tight-jail
Execute untrusted JavaScript securely.

Example usage:
```javascript
var j = new tightjail.JailContext();

j.eval('5 * 7').then(x => {
    console.log(x);

    j.exec('function somefun(a,b,c) { return a * (b + c); }');

    return j.call('somefun',[4,5,6]);
}).then(x => {
    console.log(x);
    j.close();
});
```

or using asynchronous functions:
```javascript
async function doit() {
    var j = new tightjail.JailContext();

    try {
        console.log(await j.eval('5 * 7'));

        j.exec('function somefun(a,b,c) { return a * (b + c); }');
        console.log(await j.call('somefun',[4,5,6]));
    } finally {
        j.close();
    }
}

doit();
```

A jailed execution context is created by creating a new instance of `JailContext`. Each context is completely isolated from each other and runs in its own thread.

Tight-jail has three variants that have a common interface:

### A Variant for Browsers:
Code is executed in a Web Worker inside an IFRAME.

### A Variant for Node Using an Auxiliary Executable:
The library spawns an executable that executes the code. The executable uses
the same JavaScript engine that Node does (V8) but lacks any of Node's
libraries.

Although it should not be possible to for the auxiliary executable to do
anything except perform calculations, it is still strongly recommended that a
separate user account is created for the executable (use the `setProcUser`
function to have the executable run under that account), so that a bug in the
executable or the V8 library cannot be used to gain control of the system.

### A Variant for Node.js Using a Second Node Process:
This is similar to the second variant, but uses another Node script as the
auxiliary executable. Unlike the second variant, this gives the code access to
Node's libraries.

You must use `setProcUser` to run the jail process under an unprivileged user
account. Otherwise the "jailed" code will be free to wreak havoc on the system.


## Module and WASM Support

Code running in the jail has access to a global function `jimport`. The function
takes a string argument and returns a promise. Normally the promise always
rejects with a "module not found" error, but `setModuleLoader` can be called
(from outside the jail only) to allow module importing. `setModuleLoader` takes
up to three arguments: `loader`, `getID` and `maxCacheQty`. `loader` is the
module loading function, `getID` specifies the module name normalizer and
`maxCacheQty` specifies the maximum number of modules to keep in the cache. When
`jimport` is called from inside the jail and the given argument doesn't
correspond to a cached module, the argument is passed to `loader` which is
expected to return a string, a buffer (an instance of `ArrayBuffer` when using
the browser variant and `Buffer` when using one of the Node.js variants), `null`
or a promise that resolves to one of the prior three.

* A result of `null` will cause `jimport` to reject with a "module not found"
  error.
* A string result is run as JavaScript code with the variable `exports` set to
  an empty object. `jimport` will resolve to whatever value `exports` has, after
  the code is run. This is similar to how Node.js modules work.

  Note that in the browser variant, the module code is actually run in a
  function, in the same namespace as all other code that is run in the same
  `JailContext`, thus modules are able to manipulate variables and call
  functions defined using `JailContext.eval`, although this behaviour should not
  be relied upon. This is not the case with the other variants, where modules
  get their own global namespace.

* A buffer containing a WASM binary. `jimport` resolves the compiled and
  instantiated module.

If `getID` is specified, instead of using the argument from `jimport` directly,
it is passed to `getID` and its result is either retrieved from the cache or
passed to `loader` (`getID` may also return a promise). This is useful for when
the same module can be identified by different strings (e.g. because of
case-insensitivity).

If either `loader` or `getID` throws an exception, the exception is converted to
a string and `jimport` rejects with an instance of `JailImportError` containing
the string value.

There are two levels of cache. Each `JailContext` has its own cache of
instantiated modules, thus importing the same module in the same context will
yield the same instance. There is also a global cache, storing up to
`maxCacheQty` entries (by default 50). The global cache allows multiple contexts
to instantiate the same module without compiling them again. When the maximum
number of cache entries is exceeded, the entry that has gone the longest without
being instantiated, will be released. This has no effect on instances of
modules. Module instances are not freed until the context in which they are
loaded, is destroyed. The cache can be cleared with `purgeCache`.

Because `jimport` returns a promise, the recommended way to use modules is to
place the call to `jimport` and the code that uses the module inside an `async`
function and call the function with `JailContext.call(func,args,true)` (make sure the third argument is `true`).

Example usage:
```javascript
function loader(m) {
    switch(m) {
    case "A":
        return "exports.square = x => x * x;"
    }
    return null;
}

tightjail.setModuleLoader(loader);

(async function () {
    var j = new tightjail.JailContext();

    try {
        j.exec('
            var mod = null;
            async function main() { mod = await jimport("A"); }
            function nineSquared() {
                return mod.square(9);
            }');

        /* await is required because "main" returns a promise and promises
        are not guaranteed to be resolved in any particular order */
        await j.call("main",[],true);

        console.log(await j.call("nineSquared"));
    } finally {
        j.close();
    }
})();

```


## Compiling from Source
To build the auxiliary executable, a C++ compiler and the libuv and V8
libraries are required.

The following variables may be specified either as command line arguments or
as environment variables:

`CPPFLAGS` - Extra flags for the preprocessor.

`CXX` - Path to the C++ compiler. Unlike in most build systems, the build
script does not use seperate commands to compile and link, thus there is no
variable to specify a linker command.

`CXXFLAGS` - Extra flags for compilation.

`LDFLAGS` - Extra flags for linking.

`LDLIBS` - The linker flags specifying the libraries to link to. This
overrides the default and thus should specify all needed libraries, if
specified at all.

`NAME_FLAG` - The flag to specify the compiler's output file name (e.g. `-o`
when using GCC). This is set automatically and is only needed when `TOOLS` is
set to `none`.

`RUN_CHECKS` - Whether to run some basic checks against incorrect settings
(default: true). This only applies if `TOOLS` is not set to `none`.

`TOOLS` - One of `clang`, `gcc`, `msvc` or `none`. This specifies which
compiler and flags to use. `clang`, `gcc` and `msvc` correspond to `clang++`,
`g++` and `cl.exe` respectively. `none` means no tool-set and requires that
`CXX` and any other necessary flags are specified.

`V8_BUILD_DIR` - Normally, the build script assumes V8 is installed
system-wide. If this variable is specified, it should be the path where V8 was
built (not the V8 source path). Setting this variable will also cause the
required files from V8 to be copied to the tight-jail installation path.

If building V8 from source, the default configuration is not suitable for
embedding. It *must* be configured with either `is_component_build=true` or
`v8_monolithic=true v8_use_external_startup_data=false`.

If you wish to build using MSVC, you must specify `is_component_build=true`.
Otherwise a static version of V8 is created. The V8 build system uses Clang to
compile the library, and a static library compiled with Clang is not usable by
MSVC.


## API

### Common Syntax

#### class `JailContext`

##### `JailContext.prototype.constructor(autoClose=false)`

---------------------------------------------------

##### `JailContext.prototype.eval(code)`

Return a promise to evaluate a string containing JavaScript code and
return the result.

The code must evaluate to something that can be converted to JSON.

---------------------------------------------------

##### `JailContext.prototype.exec(code)`

Return a promise to execute a string containing JavaScript code.

---------------------------------------------------

##### `JailContext.prototype.call(func,args=[],resolve_async=false)`

Return a promise to execute a JavaScript function `func` with arguments `args`.

`func` must be a string and the name of a function defined in the given context
(or a built-in function).

---------------------------------------------------

##### `JailContext.prototype.execURI(uri)`

Return a promise to execute a remote JavaScript file.

---------------------------------------------------

##### `JailContext.prototype.close()`

Destroy the connection to the jail process.

This will cause pending requests to be rejected with an instance of
`ClosedJailError`

---------------------------------------------------

##### exception `JailError` extends `Error`

Base class for jail errors.

---------------------------------------------------

##### exception `InternalJailError` extends `JailError`

An error occurred in the jail IFRAME (in the browser-side implementation) or the
jail process (in the Node implementation).

---------------------------------------------------

##### exception `DisconnectJailError` extends `JailError`

The connection to the jail was lost.

---------------------------------------------------

##### exception `ClosedJailError` extends `DisconnectJailError`

The connection was terminated manually.

---------------------------------------------------

##### exception `ClientError` extends `Error`

The jailed code threw an uncaught exception.

Unlike the other exceptions, this does not inherit from JailError.

---------------------------------------------------

#### Functions

##### `setModuleLoader(loader,getID=null,maxCacheQty=null)`

Set the module loading callbacks that are called when `jimport` is called from
jailed code.

`loader` is expected to take one string argument and return a string containing
a JavaScript module, a buffer (an instance of `ArrayBuffer` when using the
browser variant and `Buffer` when using one of the Node.js variants) containing
a WASM module or `null` to indicate no such module exists. Passing a value of
`null` for `loader` is the same as passing `x => null`.

`getID` is expected to take one string argument and return another string value
or `null`. This function is used to translate all values given to `jimport`, to
allow different strings to map to the same module. Passing a value of `null` for
`getID` is the same as passing the identity function (`x => x`).

`maxCacheQty` is expected to be an integer or `null`. Passing a value of `null`
is the same as passing the default value (50). When the number of cache items
exceeds this value, the entry that has gone the longest without being
instantiated, will be released. This has no effect on instances of modules.
Module instances are not freed until the corresponding context is closed.

---------------------------------------------------

##### `purgeCache(items=null)`

Remove items from the cache.

If `items` is null, all items are removed from the cache. Otherwise it should be
an iterable value where each element corresponds to a value that was previously
given to the `loader` function specified by `setModuleLoader`. Elements that
don't correspond to any cache item are ignored.

This does not unload already loaded module instances. Module instances are not
freed until the corresponding context is closed.

---------------------------------------------------

### Node-Specific Syntax

#### class `JailConnection`

A connection to the node process.

While `JailContext` binds a single context to a thread of execution,
`JailConnection` allows multiple contexts to be created for the same thread.
If concurrent execution is not needed, this can save memory.

The interface is similar to that of `jailContext` except `eval`, `exec`, `call`
and `execURI` take an optional `context`

---------------------------------------------------

##### `JailConnection.prototype.constructor(autoClose=false)`

---------------------------------------------------

##### `JailConnection.prototype.createContext()`

Return a promise to create a new JavaScript context and return its ID.

---------------------------------------------------

##### `JailConnection.prototype.destroyContext(context)`

Return a promise to destroy a previously created context.

`context` must be an ID create by `createContext()` and not an instance of
`JailContext`.

---------------------------------------------------

##### `JailConnection.prototype.eval(code,context=null)`

Return a promise to evaluate a string containing JavaScript code and
return the result.

The code must evaluate to something that can be converted to JSON.

---------------------------------------------------

##### `JailConnection.prototype.exec(code,context=null)`

Return a promise to execute a string containing JavaScript code.

---------------------------------------------------

##### `JailConnection.prototype.call(func,args=[],context=null,resolve_async=false)`

Return a promise to execute a JavaScript function `func` with arguments `args`.

`func` must be a string and the name of a function defined in the given context
(or a built-in function).

---------------------------------------------------

##### `JailConnection.prototype.execURI(uri,context=null)`

Return a promise to execute a remote JavaScript file.

---------------------------------------------------

##### `JailConnection.prototype.close()`

Destroy the connection to the jail process.

This will cause pending requests to be rejected with an instance of
`ClosedJailError`

---------------------------------------------------

#### Functions

##### `setProcUser(uid=null,gid=null)`

Set the user ID and group ID that will be used by the jail process.

This does not affect a jail process that has already started.

---------------------------------------------------

##### `shutdown()`

Shut down the jail process.

If a jail process is not running, this has no effect.

Creating a new instance of `JailConnection` or `JailContext` after calling
`shutdown()` will re-launch the process.

---------------------------------------------------
