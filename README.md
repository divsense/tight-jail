# tight-jail
Execute untrusted JavaScript securely.

Example usage:
```javascript
var j = new tightjail.JailContext();

j.eval('5 * 7').then(x => {
    console.log(x);

    return j.exec('function somefun(a,b,c) { return a * (b + c); }');
}).then(() => {
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

        await j.exec('function somefun(a,b,c) { return a * (b + c); }');
        console.log(await j.call('somefun',[4,5,6]));
    } finally {
        j.close();
    }
}

doit();
```

Tight-jail has three variants that have the same interface:

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

##### `JailContext.prototype.call(func,args=[])`

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

##### `JailConnection.prototype.call(func,args=[],context=null)`

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
