# tight-jail
A simpler to use and cleaner JS sandbox than jailed.js. Features: pure fp, builds single file to each specific need, cleaner more robust, etc


## API

### Common Syntax

#### class `JailContext`

---------------------------------------------------

##### `constructor(autoClose=false)`

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

##### exception `RequestJailError` extends `JailError`

The request was invalid or had the wrong format.

---------------------------------------------------

##### exception `ResultJailError` extends `JailError`

The result was invalid or had the wrong format.

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


### Node-Specific Syntax

#### class `JailContext`

---------------------------------------------------

##### `constructor(autoClose=false)`

---------------------------------------------------

##### `JailContext.prototype.createContext()`

Return a promise to create a new JavaScript context and return its ID.

---------------------------------------------------

##### `JailContext.prototype.destroyContext()`

Return a promise to destroy a previously created context.

---------------------------------------------------

##### `JailContext.prototype.eval(code,context=null)`

Return a promise to evaluate a string containing JavaScript code and
return the result.

The code must evaluate to something that can be converted to JSON.

---------------------------------------------------

##### `JailContext.prototype.exec(code,context=null)`

Return a promise to execute a string containing JavaScript code.

---------------------------------------------------

##### `JailContext.prototype.call(func,args=[],context=null)`

Return a promise to execute a JavaScript function `func` with arguments `args`.

---------------------------------------------------

##### `JailContext.prototype.execURI(uri,context=null)`

Return a promise to execute a remote JavaScript file.

---------------------------------------------------

##### `JailContext.prototype.close()`

Destroy the connection to the jail process.

This will cause pending requests to be rejected with an instance of
`ClosedJailError`

---------------------------------------------------

##### `setProcUser(uid=null,gid=null)`

---------------------------------------------------

##### `shutdown()`

Shut down the jail process.

If a jail process has not been spawned, this has no effect.

Creating a new instance of `JailConnection` or `JailContext` after calling `shutdown()` will
re-launch the process.

---------------------------------------------------

##### exception `MemoryJailError` extends `InternalJailError`

The jail process didn't have enough memory to complete an action.

---------------------------------------------------

