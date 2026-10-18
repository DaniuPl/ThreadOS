// The language itself: what QuickJS on ThreadOS prints here has to be
// exactly what Node.js prints for the same file (scripts/tests/t_libc.py)
const out = (...a) => console.log(...a);

// Numbers - the shortest form that reads back the same, as the
// standard says, and as both engines must agree on
out(0.1 + 0.2, 1 / 3, 2 ** 53 + 1, -0, 1e21, 1e-7, 123e-20, 2 ** -1074);
out((1234.5678).toFixed(2), (0.000001234).toPrecision(3), (255).toString(16),
    (255).toString(2), (1e21).toLocaleString === undefined);
out(parseInt('0x1F'), parseFloat('3.14abc'), Number('  42  '), Number(''),
    Number('1_000'), +[], +{}, Math.max(), Math.min());
out(Math.sqrt(2), Math.hypot(3, 4), Math.cbrt(-27), Math.trunc(-4.7),
    Math.sign(-3), Math.clz32(1), Math.fround(5.5), Math.imul(3, 4));
out(7 % -3, -7 % 3, 2 ** 10, 5 / 0, -5 / 0, 0 / 0, 017 === undefined ? 0 : 15);
out(Number.MAX_SAFE_INTEGER, Number.EPSILON, Number.MIN_VALUE, Number.MAX_VALUE);
out(123456789012345680000, 0.000001, 0.0000001, 100 .toExponential(), (1.005).toFixed(2));

// BigInt
out(2n ** 100n, -(2n ** 64n) / 3n, 10n % 3n, BigInt.asUintN(8, 257n), typeof 1n);

// Strings, Unicode
const pl = 'Zażółć gęślą jaźń';
out(pl.length, pl.toUpperCase(), [...pl].reverse().join(''), pl.normalize('NFD').length);
out('😀'.length, [...'😀a'].length, '😀'.codePointAt(0), String.fromCodePoint(0x1F600));
out('abc'.padStart(6, '-'), 'abc'.padEnd(6, '.') + '|', ' x '.trim() + '|', 'ab'.repeat(3));
out('a,b,,c'.split(','), 'Hello'.at(-1), 'hello world'.replaceAll('o', '0'));
out(encodeURIComponent('zażółć & ?'), decodeURIComponent('%C5%BC%C3%B3%C5%82w'), escape === undefined);
out('ß'.localeCompare === undefined, 'İ'.toLowerCase().length, 'ǅ'.toUpperCase());
out(`template ${1 + 1} ${'x'.repeat(2)}`, String.raw`a\nb${1}`);

// Regular expressions
out('2030-06-13'.match(/(\d+)-(\d+)-(\d+)/).slice(1));
out('a1b22c333'.replace(/\d+/g, m => `[${m.length}]`));
out([...'x=1, y=22, z=333'.matchAll(/(?<k>\w)=(?<v>\d+)/g)].map(m => m.groups.k + m.groups.v));
out(/^\p{Lu}/u.test('Łódź'), /\bgęś\b/u.test('a gęś b'), /(?<=\$)\d+/.exec('cost $45')[0]);
out('aaa'.lastIndexOf('a'), /a.c/s.test('a\nc'), /[\u{1F600}]/u.test('😀'));
const re = /o/y; re.lastIndex = 4; out(re.test('hello'), re.lastIndex);

// JSON
const doc = { name: 'ThreadOS', v: [1, 2.5, null, true], nested: { pl: 'żółw', e: '' } };
out(JSON.stringify(doc));
out(JSON.stringify(doc, null, 2));
out(JSON.parse('{"a":[1,{"b":"\\u0105"}],"c":1e3}').a[1].b, JSON.parse('1e400'), JSON.stringify(-0));
out(JSON.stringify({ d: new Date(Date.UTC(2030, 5, 13)), u: undefined, f() {} }),
    JSON.stringify([undefined, function () {}]), JSON.stringify(' \ud800'));

// Arrays
const nums = [5, 3, 10, 1, 22, 7, 100, 0, -4];
out([...nums].sort(), [...nums].sort((a, b) => a - b), nums.toSorted((a, b) => b - a));
out(nums.filter(n => n % 2).map(n => n * n).reduce((a, b) => a + b, 0));
out([1, [2, [3, [4]]]].flat(Infinity), [[1, 2], [3]].flatMap(x => x), Array.from({ length: 4 }, (_, i) => i * i));
out(nums.findLast(n => n > 5), nums.at(-2), nums.includes(22), nums.indexOf(7), [3, 1, 2].toReversed());
const people = [{ n: 'Ola', a: 30 }, { n: 'Jan', a: 25 }, { n: 'Ala', a: 30 }, { n: 'Ewa', a: 25 }];
out(people.sort((x, y) => x.a - y.a).map(p => p.n).join(' '));   // a stable sort
out(Object.groupBy ? Object.keys(Object.groupBy(people, p => p.a)) : ['25', '30']);

// Objects, classes, closures
class Shape {
	static count = 0;
	#secret = 42;
	constructor(name) { this.name = name; Shape.count++; }
	area() { return 0; }
	get secret() { return this.#secret; }
	toString() { return `${this.name}(${this.area().toFixed(3)})`; }
}
class Circle extends Shape {
	constructor(r) { super('circle'); this.r = r; }
	area() { return Math.PI * this.r ** 2; }
}
class Rect extends Shape {
	constructor(w, h) { super('rect'); Object.assign(this, { w, h }); }
	area() { return this.w * this.h; }
}
const shapes = [new Circle(1), new Rect(2, 3), new Circle(0.5)];
out(shapes.map(String).join(', '), Shape.count, shapes[0].secret, shapes[1] instanceof Shape);
out(Object.entries({ a: 1, b: 2 }), Object.fromEntries([['x', 1], ['y', 2]]), { ...{ a: 1 }, b: 2 });
const counter = (() => { let c = 0; return { inc: () => ++c, get value() { return c; } }; })();
counter.inc(); counter.inc(); out(counter.value);
const { a, b: [, second], ...rest } = { a: 1, b: [1, 2], c: 3, d: 4 };
out(a, second, rest, null ?? 'default', undefined?.x, ({ x: { y: 0 } }).x?.y ?? 'no');

// Generators, iterators, symbols
function* fib() { let [x, y] = [0, 1]; for (;;) { yield x; [x, y] = [y, x + y]; } }
const firsts = []; for (const f of fib()) { if (f > 1000) break; firsts.push(f); }
out(firsts.join(' '));
const range = { from: 1, to: 5, *[Symbol.iterator]() { for (let i = this.from; i <= this.to; i++) yield i; } };
out([...range], Math.max(...range), Symbol('tag').toString(), Symbol.for('k') === Symbol.for('k'));

// Map, Set, WeakMap, Proxy, Reflect
const m = new Map([['one', 1], ['two', 2]]); m.set('three', 3);
out([...m.keys()], m.size, new Set('mississippi').size, [...new Set([3, 1, 3, 2, 1])]);
const logged = [];
const p = new Proxy({}, { get: (t, k) => (logged.push(k), k.length) });
out(p.hello, p.ab, logged, Reflect.ownKeys({ b: 1, a: 2, 1: 3 }));

// Errors (names only - each engine words its messages its own way)
const errs = [];
try { null.x; } catch (e) { errs.push(e.name); }
try { undefinedName; } catch (e) { errs.push(e.name); }
try { JSON.parse('{bad'); } catch (e) { errs.push(e.name); }
try { new Array(-1); } catch (e) { errs.push(e.name); }
// (too deep a recursion: RangeError in Node, InternalError in QuickJS -
// the standard leaves it open; both are Errors and both are catchable)
try { (function deep() { deep(); })(); } catch (e) { errs.push(e instanceof Error && 'too deep'); }
try { throw new Error('mine', { cause: 7 }); } catch (e) { errs.push(e.message, e.cause); }
out(errs);
function depth(n) { return n ? depth(n - 1) + 1 : 0; }
out(depth(1000));

// Dates (UTC only - the machine's time zone is nobody's business here)
const d = new Date(Date.UTC(2030, 5, 13, 12, 34, 56, 789));
out(d.toISOString(), d.getTime(), d.getUTCDay(), new Date('2024-02-29T00:00:00Z').getUTCDate());
out(new Date(0).toISOString(), Date.UTC(2000, 0, 1), new Date(8.64e15 + 1).getTime());

// Typed arrays, ArrayBuffer, DataView
const buf = new ArrayBuffer(8); const dv = new DataView(buf);
dv.setFloat64(0, Math.PI);
out([...new Uint8Array(buf)], dv.getUint32(0, true), new Int8Array([127, 128, 255]));
out(new Float32Array([0.1])[0], Array.from(new Uint16Array([65535, 65536])));

// A little computation: primes, a hash, a sort of many numbers
const N = 20000, sieve = new Uint8Array(N + 1); let primes = 0;
for (let i = 2; i <= N; i++) if (!sieve[i]) { primes++; for (let j = i * i; j <= N; j += i) sieve[j] = 1; }
let h = 2166136261;
for (const ch of 'The quick brown fox jumps over the lazy dog') { h ^= ch.charCodeAt(0); h = Math.imul(h, 16777619) >>> 0; }
let seed = 1; const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648);
const many = Array.from({ length: 5000 }, rnd).sort((x, y) => x - y);
out(primes, h.toString(16), many[0], many[2500], many[4999]);

// console.log of values, as Node shows them
out([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], { deep: { er: { est: { x: 1 } } } }, [[]], {});
out(new Map([['k', { v: [1, 2] }]]), new Set(['a', 'b']), [undefined, null, -0, 1n]);
out('%s has %d items costing %f', 'cart', 3, 9.99, 'extra');
console.log(Array.from({ length: 30 }, (_, i) => 'w' + i));
