(function () {
// Hosted JVM Java access through the JNI Invocation/Reflection APIs.
// No ART layouts, game names, identity values, or fabricated method results.
// Embedded frontend for Venture's JNI/JVMTI method bridge.
var VentureJava = (function () {
  'use strict';
  const pointer = Process.pointerSize;
  const NULL = ptr(0);
  const classes = new Map(), functions = new Map(), members = new Map();
  const liveHooks = new Map();
  let bridge;
  const slots = {
    FindClass: 6, FromReflectedMethod: 7, FromReflectedField: 8,
    Throw: 13, ThrowNew: 14, ExceptionOccurred: 15, ExceptionDescribe: 16, ExceptionClear: 17,
    PushLocalFrame: 19, PopLocalFrame: 20, NewGlobalRef: 21,
    DeleteGlobalRef: 22, NewObjectA: 30, GetObjectClass: 31, IsInstanceOf: 32, GetMethodID: 33,
    GetStaticMethodID: 113, NewString: 163, GetStringLength: 164, GetStringChars: 165, ReleaseStringChars: 166,
    NewStringUTF: 167, GetStringUTFChars: 169,
    ReleaseStringUTFChars: 170, GetArrayLength: 171, GetObjectArrayElement: 173,
    ExceptionCheck: 228
  };
  const primitives = {
    void: ['V', 'void', 9], boolean: ['Z', 'uint8', 1], byte: ['B', 'int8', 2],
    char: ['C', 'uint16', 3], short: ['S', 'int16', 4], int: ['I', 'int', 5],
    long: ['J', 'int64', 6], float: ['F', 'float', 7], double: ['D', 'double', 8]
  };

  function environment() {
    const module = Process.findModuleByName('libart.so');
    if (module === null) throw new Error('No guest JNI invocation provider');
    const address = module.getExportByName('JNI_GetCreatedJavaVMs');
    const output = Memory.alloc(pointer), count = Memory.alloc(4);
    const enumerate = new NativeFunction(address, 'int', ['pointer', 'int', 'pointer']);
    if (enumerate(output, 1, count) !== 0 || count.readS32() !== 1)
      throw new Error('Hosted JVM unavailable');
    const vm = output.readPointer(), table = vm.readPointer(), result = Memory.alloc(pointer);
    let status = new NativeFunction(table.add(6 * pointer).readPointer(), 'int',
      ['pointer', 'pointer', 'int'])(vm, result, 0x10006);
    if (status === -2) status = new NativeFunction(table.add(7 * pointer).readPointer(), 'int',
      ['pointer', 'pointer', 'pointer'])(vm, result, NULL);
    if (status !== 0) throw new Error('JNI attachment failed: ' + status);
    return result.readPointer();
  }
  function invoke(slot, result, types, args) {
    const env = environment(), key = slot + ':' + result + ':' + types.join(',');
    if (!functions.has(key)) functions.set(key, new NativeFunction(
      env.readPointer().add(slot * pointer).readPointer(), result, ['pointer'].concat(types)));
    return functions.get(key).apply(null, [env].concat(args));
  }
  function jni(name, result, types, args) { return invoke(slots[name], result, types, args); }
  function scoped(fn) {
    if (jni('PushLocalFrame', 'int', ['int'], [128]) !== 0) check();
    try { return fn(); }
    finally { jni('PopLocalFrame', 'pointer', ['pointer'], [NULL]); }
  }
  function check() {
    if (jni('ExceptionCheck', 'uint8', [], [])) {
      const exception = jni('ExceptionOccurred', 'pointer', [], []);
      jni('ExceptionClear', 'void', [], []);
      const error = new Error('Hosted JNI operation raised a Java exception');
      error.$javaException = global(exception);
      const retained = error.$javaException;
      Script.bindWeak(error, function () { jni('DeleteGlobalRef', 'void', ['pointer'], [retained]); });
      throw error;
    }
  }
  function global(value) {
    if (value.isNull()) return value;
    const result = jni('NewGlobalRef', 'pointer', ['pointer'], [value]); check(); return result;
  }
  function clazz(name) {
    if (!classes.has(name)) {
      const value = jni('FindClass', 'pointer', ['pointer'], [Memory.allocUtf8String(name.replace(/\./g, '/'))]);
      check(); if (value.isNull()) throw new Error('Java class unavailable: ' + name);
      classes.set(name, global(value));
    }
    return classes.get(name);
  }
  function text(value) {
    if (value.isNull()) return null;
    const size = jni('GetStringLength', 'int', ['pointer'], [value]); check();
    const chars = jni('GetStringChars', 'pointer', ['pointer', 'pointer'], [value, NULL]);
    check(); if (chars.isNull()) throw new Error('JNI string access failed');
    try {
      // JNI strings have an explicit UTF-16 length; embedded NULs are data.
      const bytes = new Uint8Array(chars.readByteArray(size * 2));
      let result = '';
      for (let offset = 0; offset < size; offset += 4096) {
        const units = [];
        for (let i = offset; i < Math.min(size, offset + 4096); ++i)
          units.push(bytes[2 * i] | (bytes[2 * i + 1] << 8));
        result += String.fromCharCode.apply(null, units);
      }
      return result;
    }
    finally { jni('ReleaseStringChars', 'void', ['pointer', 'pointer'], [value, chars]); }
  }
  function method(owner, name, signature, isStatic) {
    const id = jni(isStatic ? 'GetStaticMethodID' : 'GetMethodID', 'pointer',
      ['pointer', 'pointer', 'pointer'], [owner, Memory.allocUtf8String(name), Memory.allocUtf8String(signature)]);
    check(); if (id.isNull()) throw new Error('JNI method unavailable: ' + name + signature); return id;
  }
  function rawCall(object, owner, name, signature, kind, isStatic, args) {
    return call({id: method(owner, name, signature, isStatic), result: kind, isStatic}, object, owner, args || NULL);
  }
  function call(info, object, owner, args) {
    const primitive = primitives[info.result], kind = primitive ? primitive[2] : 0;
    const isStatic = info.isStatic || info.boundReceiver;
    if (info.boundReceiver) {
      const bound = Memory.alloc((info.parameters.length + 1) * 8);
      bound.writePointer(object);
      if (info.parameters.length) Memory.copy(bound.add(8), args, info.parameters.length * 8);
      args = bound;
    }
    const slot = (isStatic ? 116 : 36) + kind * 3;
    const result = invoke(slot, primitive ? primitive[1] : 'pointer', ['pointer', 'pointer', 'pointer'],
      [info.invokeOwner || (isStatic ? owner : object), info.id, args]);
    check(); return result;
  }
  function list(array) {
    const size = jni('GetArrayLength', 'int', ['pointer'], [array]); check(); const result = [];
    for (let i = 0; i < size; ++i) {
      result.push(jni('GetObjectArrayElement', 'pointer', ['pointer', 'int'], [array, i])); check();
    }
    return result;
  }
  function className(type) { return text(rawCall(type, clazz('java.lang.Class'), 'getName', '()Ljava/lang/String;', 'java.lang.String', false)); }
  function declared(type, name, signature) {
    return list(rawCall(type, clazz('java.lang.Class'), name, signature, 'array', false));
  }
  function metadata(name) {
    if (members.has(name)) return members.get(name);
    const type = clazz(name), result = {methods: new Map(), constructors: [], fields: new Map()};
    // Include inherited public members, then declared members so overrides win.
    for (const reflection of declared(type, 'getMethods', '()[Ljava/lang/reflect/Method;')
      .concat(declared(type, 'getDeclaredMethods', '()[Ljava/lang/reflect/Method;'))) {
      const api = clazz('java.lang.reflect.Method');
      const key = text(rawCall(reflection, api, 'getName', '()Ljava/lang/String;', 'java.lang.String', false));
      const parameters = list(rawCall(reflection, api, 'getParameterTypes', '()[Ljava/lang/Class;', 'array', false)).map(className);
      const returns = className(rawCall(reflection, api, 'getReturnType', '()Ljava/lang/Class;', 'java.lang.Class', false));
      const modifiers = rawCall(reflection, api, 'getModifiers', '()I', 'int', false);
      const id = jni('FromReflectedMethod', 'pointer', ['pointer'], [reflection]); check();
      const info = {id, parameters, result: returns, isStatic: (modifiers & 8) !== 0};
      if (!result.methods.has(key)) result.methods.set(key, new Map());
      result.methods.get(key).set(parameters.join(';'), info);
    }
    for (const reflection of declared(type, 'getDeclaredConstructors', '()[Ljava/lang/reflect/Constructor;')) {
      const parameters = list(rawCall(reflection, clazz('java.lang.reflect.Constructor'),
        'getParameterTypes', '()[Ljava/lang/Class;', 'array', false)).map(className);
      const id = jni('FromReflectedMethod', 'pointer', ['pointer'], [reflection]); check();
      result.constructors.push({id, parameters, result: name, constructor: true});
    }
    for (const reflection of declared(type, 'getFields', '()[Ljava/lang/reflect/Field;')
      .concat(declared(type, 'getDeclaredFields', '()[Ljava/lang/reflect/Field;'))) {
      const api = clazz('java.lang.reflect.Field');
      const key = text(rawCall(reflection, api, 'getName', '()Ljava/lang/String;', 'java.lang.String', false));
      const fieldType = className(rawCall(reflection, api, 'getType', '()Ljava/lang/Class;', 'java.lang.Class', false));
      const modifiers = rawCall(reflection, api, 'getModifiers', '()I', 'int', false);
      const id = jni('FromReflectedField', 'pointer', ['pointer'], [reflection]); check();
      result.fields.set(key, {id, type: fieldType, isStatic: (modifiers & 8) !== 0});
    }
    if (name === 'java.lang.Runtime') {
      const loaderOwner = clazz('com.venture.runtime.NativeLoader');
      for (const key of ['loadLibrary0', 'load0']) {
        if (!result.methods.has(key)) result.methods.set(key, new Map());
        for (const parameters of [
          ['java.lang.ClassLoader', 'java.lang.String'],
          ['java.lang.Class', 'java.lang.String'],
          ['java.lang.ClassLoader', 'java.lang.Class', 'java.lang.String']
        ]) {
          const signature = '(Ljava/lang/Runtime;' + parameters.map(type => 'L' + type.replace(/\./g, '/') + ';').join('') + ')V';
          const id = method(loaderOwner, key, signature, true);
          result.methods.get(key).set(parameters.join(';'), {
            id, parameters, result: 'void', isStatic: false,
            invokeOwner: loaderOwner, boundReceiver: true
          });
        }
      }
    }
    members.set(name, result); return result;
  }
  function reference(value) {
    if (value === null || value === undefined) return NULL;
    if (typeof value === 'string') {
      const chars = Memory.alloc(Math.max(2, value.length * 2));
      const bytes = new Uint8Array(value.length * 2);
      for (let i = 0; i < value.length; ++i) {
        const unit = value.charCodeAt(i);
        bytes[2 * i] = unit & 255; bytes[2 * i + 1] = unit >>> 8;
      }
      chars.writeByteArray(bytes);
      const s = jni('NewString', 'pointer', ['pointer', 'int'], [chars, value.length]);
      check(); return s;
    }
    if (value.$handle) return value.$handle;
    throw new Error('Expected a Java object or string');
  }
  function argumentsFor(types, values) {
    const memory = Memory.alloc(Math.max(8, types.length * 8));
    types.forEach(function (type, index) {
      const out = memory.add(index * 8), value = values[index];
      switch (type) {
        case 'boolean': out.writeU8(value ? 1 : 0); break;
        case 'byte': out.writeS8(value); break;
        case 'char': out.writeU16(typeof value === 'string' ? value.charCodeAt(0) : value); break;
        case 'short': out.writeS16(value); break;
        case 'int': out.writeS32(value); break;
        case 'long': out.writeS64(value); break;
        case 'float': out.writeFloat(value); break;
        case 'double': out.writeDouble(value); break;
        default: {
          const object = reference(value);
          if (!object.isNull() && jni('IsInstanceOf', 'uint8', ['pointer', 'pointer'], [object, clazz(type)]) === 0)
            throw new TypeError('Java argument is not assignable to ' + type);
          check(); out.writePointer(object); break;
        }
      }
    });
    return memory;
  }
  function resultValue(value, type) {
    if (type === 'void') return undefined;
    if (type === 'boolean') return value !== 0;
    if (primitives[type]) return value;
    if (value.isNull()) return null;
    if (type === 'java.lang.String') return text(value);
    if (type.startsWith('[L') || type.startsWith('[[')) return list(value).map(v => resultValue(v, type.startsWith('[L') ? type.slice(2, -1) : type.slice(1)));
    return wrap(value, type, false);
  }
  function compatible(type, value) {
    if (type === 'boolean') return typeof value === 'boolean';
    if (type === 'char') return typeof value === 'number' || (typeof value === 'string' && value.length === 1);
    if (primitives[type]) return typeof value === 'number' || (type === 'long' && value instanceof Int64);
    if (value === null) return true;
    if (typeof value !== 'string' && !(value && value.$handle)) return false;
    const object = reference(value);
    const result = jni('IsInstanceOf', 'uint8', ['pointer', 'pointer'], [object, clazz(type)]) !== 0;
    check(); return result;
  }
  function hookBridge() {
    if (!bridge) {
      const module = Process.getModuleByName('libventure_java_bridge.so');
      bridge = {
        install: new NativeFunction(module.getExportByName('VentureJavaInstallHook'), 'int', ['pointer','pointer','pointer','pointer']),
        remove: new NativeFunction(module.getExportByName('VentureJavaRemoveHook'), 'int', ['pointer'])
      };
    }
    return bridge;
  }
  function valueAt(memory, type) {
    switch (type) {
      case 'boolean': return memory.readU8() !== 0;
      case 'byte': return memory.readS8();
      case 'char': return String.fromCharCode(memory.readU16());
      case 'short': return memory.readS16();
      case 'int': return memory.readS32();
      case 'long': return memory.readS64();
      case 'float': return memory.readFloat();
      case 'double': return memory.readDouble();
      default: return resultValue(memory.readPointer(), type);
    }
  }
  function setHook(owner, name, info, replacement) {
    if (info.constructor === true) throw new Error('Constructor replacement is not supported by the hosted JVM bridge');
    const api = hookBridge();
    if (replacement === null) {
      if (info.hook) {
        const status = api.remove(info.id);
        if (status !== 0) throw new Error('JVM hook removal failed: ' + status);
        info.hook = null;
      }
      return;
    }
    if (typeof replacement !== 'function') throw new TypeError('Java implementation must be a function or null');
    const token = Memory.alloc(pointer), key = token.toString();
    const record = {token, replacement};
    record.release = new NativeCallback(function () {
      // The native backend signals only after every borrowed callback returns.
      setImmediate(function () { liveHooks.delete(key); });
    }, 'void', ['pointer']);
    record.callback = new NativeCallback(function (env, receiver, values, output) {
      try {
        const args = info.parameters.map((type, index) => valueAt(values.add(8 * (index + (info.boundReceiver ? 1 : 0))), type));
        const target = info.isStatic ? wrap(owner, name.slice(0, name.lastIndexOf('.')), true)
                                     : wrap(info.boundReceiver ? values.readPointer() : receiver, name.slice(0, name.lastIndexOf('.')), false);
        const value = replacement.apply(target, args);
        if (info.result !== 'void') Memory.copy(output, argumentsFor([info.result], [value]), 8);
        return 1;
      } catch (error) {
        if (error && error.$javaException) jni('Throw', 'int', ['pointer'], [error.$javaException]);
        else jni('ThrowNew', 'int', ['pointer','pointer'], [clazz('java.lang.RuntimeException'),
          Memory.allocUtf8String('Hosted Java replacement failed')]);
        return 2;
      }
    }, 'int', ['pointer','pointer','pointer','pointer','pointer']);
    liveHooks.set(key, record);
    const status = api.install(info.id, record.callback, token, record.release);
    if (status !== 0) {
      liveHooks.delete(key);
      throw new Error('JVM hook installation failed: ' + status);
    }
    info.hook = record;
  }
  function callable(owner, object, name, choices) {
    function selected(args) {
      const matches = choices.filter(info => info.parameters.length === args.length && info.parameters.every((t, i) => compatible(t, args[i])));
      if (matches.length !== 1) throw new Error('Choose a unique Java overload: ' + name);
      return matches[0];
    }
    function execute(info, args, receiver) {
      const target = receiver || object;
      if (!info.isStatic && info.constructor !== true && !target.isNull()) {
        const valid = jni('IsInstanceOf', 'uint8', ['pointer', 'pointer'], [target, owner]) !== 0;
        check(); if (!valid) throw new TypeError('Java receiver is not assignable to method owner');
      }
      const values = argumentsFor(info.parameters, args);
      if (info.constructor === true) {
        const value = jni('NewObjectA', 'pointer', ['pointer', 'pointer', 'pointer'], [owner, info.id, values]);
        check(); return wrap(value, info.result, false);
      }
      const answer = resultValue(call(info, receiver || object, owner, values), info.result);
      return answer;
    }
    function create(fixed) {
      const fn = function () { const args = Array.from(arguments), receiver = this && this.$handle;
        return scoped(function () { return execute(fixed || selected(args), args, receiver); }); };
      fn.overload = function () {
        const types = Array.from(arguments), match = choices.find(info => info.parameters.join(';') === types.join(';'));
        if (!match) throw new Error('Java overload unavailable: ' + name + '(' + types.join(',') + ')');
        return create(match);
      };
      Object.defineProperty(fn, 'implementation', {
        get: function () { const info = fixed || (choices.length === 1 ? choices[0] : null); return info && info.hook ? info.hook.replacement : null; },
        set: function (replacement) {
          const info = fixed || (choices.length === 1 ? choices[0] : null);
          if (!info) throw new Error('Choose a Java overload before replacing: ' + name);
          setHook(owner, name, info, replacement);
        }
      });
      return fn;
    }
    return create(null);
  }
  function wrap(handle, name, isClass) {
    if (handle.isNull()) return null;
    handle = global(handle);
    const state = {disposed: false};
    function dispose() {
      if (!state.disposed) {
        state.disposed = true;
        jni('DeleteGlobalRef', 'void', ['pointer'], [handle]);
      }
    }
    const proxy = new Proxy({$handle: handle, $className: name}, {
      get: function (object, key) {
        if (state.disposed && key !== '$dispose') throw new Error('Java wrapper is disposed');
        if (Object.prototype.hasOwnProperty.call(object, key)) return object[key];
        if (key === Symbol.toPrimitive) return function (hint) {
          const value = text(rawCall(handle, clazz('java.lang.Object'), 'toString',
            '()Ljava/lang/String;', 'java.lang.String', false));
          return hint === 'number' ? Number(value) : value;
        };
        if (key === Symbol.toStringTag) return name;
        if (typeof key === 'symbol') return undefined;
        if (key === 'then') return undefined;
        if (key === '$dispose') return dispose;
        const owner = clazz(name), data = metadata(name);
        if (key === 'class' && isClass) return wrap(owner, 'java.lang.Class', false);
        if (key === '$new' && isClass) return callable(owner, NULL, name + '.$new', data.constructors);
        if (data.fields.has(key)) {
          const field = data.fields.get(key), primitive = primitives[field.type], kind = primitive ? primitive[2] : 0;
          const target = field.isStatic ? owner : handle;
          return Object.defineProperty({}, 'value', {
            get: function () {
              const value = invoke((field.isStatic ? 145 : 95) + kind, primitive ? primitive[1] : 'pointer', ['pointer', 'pointer'], [target, field.id]);
              check(); return resultValue(value, field.type);
            }
          });
        }
        if (!data.methods.has(key)) throw new Error('Java member unavailable: ' + name + '.' + String(key));
        return callable(owner, handle, name + '.' + key, Array.from(data.methods.get(key).values()));
      }
    });
    Script.bindWeak(proxy, dispose);
    return proxy;
  }
  return {
    available: true,
    perform: function (fn) { environment(); return scoped(fn); },
    performNow: function (fn) { environment(); return scoped(fn); },
    use: function (name) { return wrap(clazz(name), name, true); }
  };
})();
Object.defineProperty(globalThis, "Java", {value: VentureJava, configurable: true});
})();
