// The language itself, for JavaScriptCore on ThreadOS: what it prints
// here has to be exactly what Node.js prints for the same file
// (scripts/tests/t_jsc.py). Only plain values are printed - the two
// shells show objects differently, but a string is a string - so where
// a structure is the answer it goes through JSON.stringify first.
const out = typeof print === 'function' ? print : console.log;
const J = v => JSON.stringify(v);

// Numbers: the shortest form that reads back the same
out(0.1 + 0.2, 1 / 3, 2 ** 53 + 1, 1e21, 1e-7, 123e-20, 2 ** -1074);
out((1234.5678).toFixed(2), (0.000001234).toPrecision(3), (255).toString(16),
    (255).toString(36), (-1.5e-9).toExponential(3), (1.005).toFixed(2));
out(parseInt('0x1F'), parseFloat('3.14abc'), Number('  42  '), Number(''),
    Number('1_000'), +[], +{}, Math.max(), Math.min());
out(Math.sqrt(2), Math.hypot(3, 4), Math.cbrt(-27), Math.trunc(-4.7),
    Math.clz32(1), Math.fround(5.5), Math.imul(0xffffffff, 5), 7 % -3, -7 % 3);
out(Math.sin(1).toFixed(12), Math.exp(1).toFixed(12), Math.atan2(1, 2).toFixed(12));
out(Number.MAX_SAFE_INTEGER, Number.EPSILON, Number.MIN_VALUE, Number.MAX_VALUE);

// BigInt
out(String(2n ** 100n), String(-(2n ** 64n) / 3n), String(BigInt.asUintN(8, 257n)),
    String(BigInt('0x' + 'f'.repeat(20))), typeof 1n);

// Strings
const pl = 'Zażółć gęślą jaźń';
out(pl.length, pl.toUpperCase(), [...pl].reverse().join(''), pl.normalize('NFD').length);
out('😀'.length, [...'😀a'].length, '😀'.codePointAt(0), String.fromCodePoint(0x1F600));
out('abc'.padStart(6, '-'), 'abc'.padEnd(6, '.') + '|', ' x '.trim() + '|',
    'ab'.repeat(3), 'Hello'.at(-1), 'hello world'.replaceAll('o', '0'));
out(J('a,b,,c'.split(',')), encodeURIComponent('zażółć & ?'),
    decodeURIComponent('%C5%BC%C3%B3%C5%82w'), String.raw`a\nb${1}`);
out('Lorem ipsum dolor'.split(' ').map(w => w[0]).join(''), 'x'.localeCompare('y'),
    'abc'.isWellFormed(), '\uD800'.toWellFormed().codePointAt(0));

// Regular expressions
const date = /(?<y>\d{4})-(?<m>\d\d)-(?<d>\d\d)/u.exec('on 2024-01-02, late');
out(date.index, date.groups.y, date.groups.d, J([...'a1b22c333'.matchAll(/\d+/g)].map(m => m[0])));
out('aBc DeF'.replace(/(\w)(\w*)/g, (_, a, b) => a.toUpperCase() + b.toLowerCase()),
    /^[\p{L}\s]+$/u.test(pl), 'x.y.z'.split(/\./).length, /(?<=\$)\d+/.exec('cost $42')[0]);
out(J('aaa'.match(/a*?/)), /a.c/s.test('a\nc'), 'abc'.search(/c/), /\bfoo\b/i.test('A FOO!'));
let bad = 'none';
try { new RegExp('[', 'g'); } catch (e) { bad = e.name; }
out(J(/(a)|(b)/d.exec('xb').indices), bad);

// Objects, classes, getters, private fields
class Shape {
    #id;
    static count = 0;
    constructor(id) { this.#id = id; Shape.count++; }
    get id() { return this.#id; }
    area() { return 0; }
    toString() { return `${this.constructor.name}#${this.#id}=${this.area()}`; }
    static isShape(x) { return #id in x; }
}
class Square extends Shape {
    constructor(id, s) { super(id); this.s = s; }
    area() { return this.s * this.s; }
}
const shapes = [new Square(1, 3), new Shape(2), new Square(3, 1.5)];
out(shapes.join(' '), Shape.count, Shape.isShape(shapes[0]), Shape.isShape({}),
    Object.getPrototypeOf(Square) === Shape);
const o = { a: 1, get b() { return this.a + 1; }, ['c' + 1]: 3, ...{ d: 4 } };
out(J(o), J(Object.entries(o)), J(Object.fromEntries([['x', 1]])), 'b' in o, o?.z?.y);
const { a, b: renamed = 9, ...rest } = o;
out(a, renamed, J(rest), J([...'ab', ...[1, 2]]));
out(J(Object.groupBy([1, 2, 3, 4, 5], n => n % 2 ? 'odd' : 'even')));

// Arrays
const arr = [5, 1, 4, 2, 3];
out(J(arr.toSorted()), J(arr), J(arr.toReversed()), J(arr.with(0, 9)), arr.findLast(n => n > 3),
    arr.at(-2), J([1, [2, [3, [4]]]].flat(Infinity)), J([1, 2].flatMap(x => [x, x * 10])));
out(J(Array.from({ length: 5 }, (_, i) => i * i)), J([3, 1, 2].sort((x, y) => y - x)),
    [1, 2, 3].reduce((s, x) => s + x, 0), J([..."hello"].entries().next().value));
const stable = [['b', 1], ['a', 2], ['b', 0], ['a', 1]].sort((x, y) => x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0);
out(J(stable));
const typed = new Float64Array([1.5, -2, 3]);
const view = new DataView(new ArrayBuffer(8));
view.setUint32(0, 0xdeadbeef);
out(J(Array.from(typed.map(x => x * 2))), view.getUint8(1), view.getInt32(0, true),
    new Uint8Array([300, -1, 1.7]).join(','), Uint16Array.BYTES_PER_ELEMENT);

// Collections, symbols, iteration
const map = new Map([[1, 'one'], ['two', 2]]);
const set = new Set('mississippi');
out(map.size, map.get('two'), J([...map.keys()]), [...set].join(''), set.has('s'));
const weak = new WeakMap([[o, 'obj']]);
out(weak.get(o), Symbol('desc').description, Symbol.for('k') === Symbol.for('k'),
    typeof Symbol.iterator, Object.getOwnPropertySymbols({ [Symbol('s')]: 1 }).length);
function* gen(n) { for (let i = 0; i < n; i++) yield i * i; return 'done'; }
const g = gen(3);
out(J([...gen(5)]), g.next().value, g.next().value, g.next().value, J(g.next()));
const iterable = { *[Symbol.iterator]() { yield* [1, 2]; yield* 'ab'; } };
out(J([...iterable]), J(Array.from(new Map([[1, 2]]).entries())));

// Functions and scope
const add = (x, y = 10, ...more) => x + y + more.length;
out(add(1), add(1, 2), add(1, 2, 3, 4), add.length, (function () { return typeof this; })(),
    (function () { return (() => arguments.length)(); })(1, 2));
function counter() { let n = 0; return { inc: () => ++n, get: () => n }; }
const c1 = counter(), c2 = counter();
c1.inc(); c1.inc(); c2.inc();
out(c1.get(), c2.get(), [1, 2, 3].map(Math.sqrt.bind(null)).map(x => x.toFixed(3)).join(' '));
const memo = new Map();
const fib = n => n < 2 ? n : memo.get(n) ?? (memo.set(n, fib(n - 1) + fib(n - 2)), memo.get(n));
out(fib(90), String(BigInt(fib(70)) * 3n));

// Errors
const caught = [];
try { null.x; } catch (e) { caught.push(e.name); }
try { undefinedName; } catch (e) { caught.push(e.name); }
try { new Array(-1); } catch (e) { caught.push(e.name); }
try { JSON.parse('{bad'); } catch (e) { caught.push(e.name); }
try { throw new Error('mine', { cause: 7 }); } catch (e) { caught.push(e.message + '/' + e.cause); }
let fin = '';
for (const x of [1, 2]) { try { if (x === 2) continue; fin += 't'; } finally { fin += 'f'; } }
out(caught.join(' '), fin, new AggregateError([1], 'agg').errors.length);

// JSON, Proxy, Reflect, dates
const parsed = JSON.parse('{"a":[1,2,{"b":null}],"c":"\\u00e9","d":1e3}');
out(J(parsed), JSON.stringify({ x: [1, { y: 2 }] }, null, 2).split('\n').length,
    J({ u: undefined, f() {}, n: NaN, d: new Date(0) }));
const logged = [];
const proxy = new Proxy({ k: 1 }, {
    get(t, p, r) { logged.push(String(p)); return Reflect.get(t, p, r); },
    has(t, p) { return p === 'magic' || p in t; },
});
out(proxy.k, 'magic' in proxy, logged.join(','), J(Reflect.ownKeys({ b: 1, a: 2, 1: 0 })));
const dt = new Date(Date.UTC(2020, 1, 29, 23, 59, 59, 999));
out(dt.toISOString(), dt.getTime(), Date.parse('2024-03-10T12:00:00Z'),
    new Date(dt.getTime() + 1).toISOString(), J(new Date(NaN)));

// Promises and async functions: the order things happen in
const order = [];
const later = (v, ms) => new Promise(r => setTimeout(() => r(v), ms));
(async () => {
    order.push('start');
    const v = await later('timer', 5);
    order.push(v);
    const all = await Promise.all([later(1, 3), Promise.resolve(2), 3]);
    order.push(J(all));
    const settled = await Promise.allSettled([Promise.reject(new Error('x')), later('ok', 1)]);
    order.push(settled.map(s => s.status).join('/'));
    for await (const x of (async function* () { yield 'a'; yield 'b'; })())
        order.push(x);
    out(order.join(' '));
})();
Promise.resolve().then(() => order.push('micro'));
order.push('sync');
