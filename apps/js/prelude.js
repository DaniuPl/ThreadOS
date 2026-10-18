/* js - what the host adds to the language, written in the language
 *
 * QuickJS is the language alone: no console, no timers, nothing that
 * reads or prints. Those are the host's, and most of them are easier
 * to say in JavaScript than in C - so they are said here, and js.c
 * runs this once before anything else, handing it the few things only
 * C can do (the 'host' object: writing to a descriptor, reading and
 * writing a file, the clock, a promise's state).
 *
 * console.log prints values the way Node.js does - the same quotes,
 * the same spacing, arrays of more than six numbers in the same
 * columns - because that is the format everybody already knows, and
 * because then a test can hold the two side by side
 * (scripts/tests/t_js.py does).
 */
(function (global, host) {
'use strict';

const keyIdent = /^[a-zA-Z_][a-zA-Z_0-9]*$/;
const breakLength = 80, compact = 3, maxArrayLength = 100;

function quote(s) {
	let q = "'";
	if (s.includes("'")) {
		if (!s.includes('"'))
			q = '"';
		else if (!s.includes('`') && !s.includes('${'))
			q = '`';
	}
	let out = '';
	for (const ch of s) {
		const c = ch.codePointAt(0);
		if (ch === q || ch === '\\')
			out += '\\' + ch;
		else if (ch === '\n')
			out += '\\n';
		else if (ch === '\t')
			out += '\\t';
		else if (ch === '\r')
			out += '\\r';
		else if (ch === '\b')
			out += '\\b';
		else if (ch === '\f')
			out += '\\f';
		else if (ch === '\v')
			out += '\\v';
		else if (c < 0x20 || c === 0x7f)
			out += '\\x' + c.toString(16).toUpperCase().padStart(2, '0');
		else
			out += ch;
	}
	return q + out + q;
}

function formatKey(k) {
	if (typeof k === 'symbol')
		return '[' + k.toString() + ']';
	return keyIdent.test(k) ? k : quote(k);
}

function fnBase(f) {
	let src = '';
	try { src = Function.prototype.toString.call(f); } catch (e) {}
	if (src.startsWith('class') && /^class\b/.test(src)) {
		let base = '[class ' + (f.name || '(anonymous)');
		const parent = Object.getPrototypeOf(f);
		if (parent && parent.name)
			base += ' extends ' + parent.name;
		return base + ']';
	}
	let kind = 'Function';
	const ctor = f.constructor && f.constructor.name;
	if (ctor === 'AsyncFunction' || ctor === 'GeneratorFunction' ||
	    ctor === 'AsyncGeneratorFunction')
		kind = ctor;
	return '[' + kind + (f.name ? ': ' + f.name : ' (anonymous)') + ']';
}

function ctorName(o) {
	let p = o;
	while (p) {
		const d = Object.getOwnPropertyDescriptor(p, 'constructor');
		if (d && typeof d.value === 'function' && d.value.name)
			return d.value.name;
		p = Object.getPrototypeOf(p);
	}
	return null;
}

function formatPrimitive(v) {
	switch (typeof v) {
	case 'string': return quote(v);
	case 'number': return Object.is(v, -0) ? '-0' : String(v);
	case 'bigint': return v + 'n';
	case 'symbol': return v.toString();
	default: return String(v);
	}
}

function isBelowBreakLength(ctx, output, start, base) {
	let total = output.length + start;
	if (total + output.length > breakLength)
		return false;
	for (const s of output) {
		total += s.length;
		if (total > breakLength)
			return false;
	}
	return base === '' || !base.includes('\n');
}

function groupArrayElements(ctx, output, value) {
	let totalLength = 0, maxLength = 0;
	let outputLength = output.length;
	const extra = maxArrayLength < output.length;
	if (extra)
		outputLength--;              /* "... more items" stays out of it */
	const sep = 2;
	const dataLen = new Array(outputLength);
	for (let i = 0; i < outputLength; i++) {
		const len = output[i].length;
		dataLen[i] = len;
		totalLength += len + sep;
		if (maxLength < len)
			maxLength = len;
	}
	const actualMax = maxLength + sep;
	if (actualMax * 3 + ctx.indent < breakLength &&
	    (totalLength / actualMax > 5 || maxLength <= 6)) {
		const averageBias = Math.sqrt(actualMax - totalLength / output.length);
		const biasedMax = Math.max(actualMax - 3 - averageBias, 1);
		const columns = Math.min(
			Math.round(Math.sqrt(2.5 * biasedMax * outputLength) / biasedMax),
			Math.floor((breakLength - ctx.indent) / actualMax),
			compact * 4, 15);
		if (columns <= 1)
			return output;
		const tmp = [], maxLineLength = [];
		for (let i = 0; i < columns; i++) {
			let lineLength = 0;
			for (let j = i; j < output.length; j += columns)
				if (dataLen[j] > lineLength)
					lineLength = dataLen[j];
			maxLineLength.push(lineLength + sep);
		}
		let padStart = true;
		if (value !== undefined)
			for (let i = 0; i < output.length; i++)
				if (typeof value[i] !== 'number' && typeof value[i] !== 'bigint') {
					padStart = false;
					break;
				}
		for (let i = 0; i < outputLength; i += columns) {
			const max = Math.min(i + columns, outputLength);
			let str = '', j = i;
			for (; j < max - 1; j++) {
				const cell = output[j] + ', ';
				str += padStart ? cell.padStart(maxLineLength[j - i])
				                : cell.padEnd(maxLineLength[j - i]);
			}
			str += padStart ? output[j].padStart(maxLineLength[j - i] - sep)
			                : output[j];
			tmp.push(str);
		}
		if (extra)
			tmp.push(output[outputLength]);
		output = tmp;
	}
	return output;
}

function reduceToSingleString(ctx, output, base, braces, isArray, depth, value) {
	const entries = output.length;
	if (isArray && entries > 6)
		output = groupArrayElements(ctx, output, value);
	if (ctx.currentDepth - depth < compact && entries === output.length) {
		const start = output.length + ctx.indent + braces[0].length +
		              base.length + 10;
		if (isBelowBreakLength(ctx, output, start, base)) {
			const joined = output.join(', ');
			if (!joined.includes('\n'))
				return (base ? base + ' ' : '') + braces[0] + ' ' + joined +
				       ' ' + braces[1];
		}
	}
	const ind = '\n' + ' '.repeat(ctx.indent);
	return (base ? base + ' ' : '') + braces[0] + ind + '  ' +
	       output.join(',' + ind + '  ') + ind + braces[1];
}

function formatProperty(ctx, obj, key, depth) {
	const d = Object.getOwnPropertyDescriptor(obj, key);
	let str;
	if (d.get || d.set)
		str = d.get && d.set ? '[Getter/Setter]' : d.get ? '[Getter]' : '[Setter]';
	else {
		ctx.indent += 2;
		str = formatValue(ctx, d.value, depth + 1, true);
		ctx.indent -= 2;
	}
	return formatKey(key) + ': ' + str;
}

function ownKeys(o, skipIndices) {
	const keys = [];
	for (const k of Object.keys(o))
		if (!skipIndices || !(String(k >>> 0) === k && (k >>> 0) !== 4294967295))
			keys.push(k);
	for (const s of Object.getOwnPropertySymbols(o)) {
		const d = Object.getOwnPropertyDescriptor(o, s);
		if (d && d.enumerable)
			keys.push(s);
	}
	return keys;
}

function formatValue(ctx, v, depth, nested) {
	if (v === null || (typeof v !== 'object' && typeof v !== 'function'))
		return nested || typeof v !== 'string' ? formatPrimitive(v) : v;

	if (ctx.seen.includes(v)) {
		let idx = ctx.circular.get(v);
		if (idx === undefined) {
			idx = ctx.circular.size + 1;
			ctx.circular.set(v, idx);
		}
		return '[Circular *' + idx + ']';
	}
	return formatRaw(ctx, v, depth);
}

function formatRaw(ctx, v, depth) {
	const name = ctorName(v);
	let base = '', braces, output = [], isArray = false;
	let keys;

	if (typeof v === 'function') {
		base = fnBase(v);
		keys = ownKeys(v, false);
		if (!keys.length)
			return base;
		braces = ['{', '}'];
	} else if (Array.isArray(v)) {
		isArray = true;
		const prefix = name !== 'Array' ? (name || '[Array: null prototype]') +
		               '(' + v.length + ') ' : '';
		braces = [prefix + '[', ']'];
		keys = ownKeys(v, true);
		if (!v.length && !keys.length)
			return braces[0] + ']';
	} else if (ArrayBuffer.isView(v) && !(v instanceof DataView)) {
		isArray = true;
		braces = [name + '(' + v.length + ') [', ']'];
		keys = ownKeys(v, true);
		if (!v.length && !keys.length)
			return braces[0] + ']';
	} else if (v instanceof Map) {
		braces = [name + '(' + v.size + ') {', '}'];
		keys = ownKeys(v, false);
		if (!v.size && !keys.length)
			return braces[0] + '}';
	} else if (v instanceof Set) {
		braces = [name + '(' + v.size + ') {', '}'];
		keys = ownKeys(v, false);
		if (!v.size && !keys.length)
			return braces[0] + '}';
	} else {
		keys = ownKeys(v, false);
		if (v instanceof Date)
			base = isNaN(v.getTime()) ? 'Invalid Date' : v.toISOString();
		else if (v instanceof RegExp)
			base = RegExp.prototype.toString.call(v);
		else if (v instanceof Error) {
			base = v.stack ? String(v) + '\n' + v.stack.replace(/\n$/, '') : String(v);
			if (!v.stack)
				base = '[' + base + ']';
			keys = keys.filter(k => k !== 'stack' && k !== 'message');
		} else if (v instanceof Promise) {
			const [state, result] = host.promiseState(v);
			braces = [name + ' {', '}'];
			output.push(state === 0 ? '<pending>' :
			            (state === 2 ? '<rejected> ' : '') +
			            formatValue(ctx, result, depth + 1, true));
		} else if (v instanceof Number || v instanceof String ||
		           v instanceof Boolean || v instanceof BigInt) {
			const prim = v.valueOf();
			base = '[' + name + ': ' + formatPrimitive(prim) + ']';
			if (v instanceof String)
				keys = ownKeys(v, true);
		}
		if (base && !keys.length)
			return base;
		if (!braces) {
			const proto = Object.getPrototypeOf(v);
			if (proto === null)
				braces = ['[Object: null prototype] {', '}'];
			else if (name === 'Object' || base)
				braces = ['{', '}'];
			else
				braces = [(name || 'Object') + ' {', '}'];
			if (!keys.length && !output.length)
				return braces[0] + '}';
		}
	}

	if (depth > ctx.depth)
		return '[' + (Array.isArray(v) ? 'Array' : name || 'Object') + ']';

	ctx.seen.push(v);
	ctx.currentDepth = depth;
	if (isArray) {
		let holes = 0, shown = 0;
		const len = v.length;
		for (let i = 0; i < len && shown < maxArrayLength; i++) {
			if (!Object.prototype.hasOwnProperty.call(v, i)) {
				holes++;
				continue;
			}
			if (holes) {
				output.push('<' + holes + ' empty item' + (holes > 1 ? 's' : '') + '>');
				shown++;
				holes = 0;
			}
			ctx.indent += 2;
			output.push(formatValue(ctx, v[i], depth + 1, true));
			ctx.indent -= 2;
			shown++;
		}
		if (holes)
			output.push('<' + holes + ' empty item' + (holes > 1 ? 's' : '') + '>');
		if (shown < len && output.length >= maxArrayLength) {
			const more = len - maxArrayLength;
			output.push('... ' + more + ' more item' + (more > 1 ? 's' : ''));
		}
	} else if (v instanceof Map) {
		ctx.indent += 2;
		for (const [k, val] of v)
			output.push(formatValue(ctx, k, depth + 1, true) + ' => ' +
			            formatValue(ctx, val, depth + 1, true));
		ctx.indent -= 2;
	} else if (v instanceof Set) {
		ctx.indent += 2;
		for (const val of v)
			output.push(formatValue(ctx, val, depth + 1, true));
		ctx.indent -= 2;
	}
	for (const k of keys)
		output.push(formatProperty(ctx, v, k, depth));
	ctx.seen.pop();

	let res = reduceToSingleString(ctx, output, base, braces, isArray, depth, v);
	const idx = ctx.circular.get(v);
	if (idx !== undefined)
		res = '<ref *' + idx + '> ' + res;
	return res;
}

function inspect(v, nested, depth = 2) {
	return formatValue({ seen: [], circular: new Map(), indent: 0,
	                     currentDepth: 0, depth }, v, 0, nested);
}

function format(args) {
	let out = '', i = 0;
	if (typeof args[0] === 'string' && args.length > 1 && args[0].includes('%')) {
		const f = args[0];
		i = 1;
		for (let p = 0; p < f.length; p++) {
			if (f[p] !== '%' || p + 1 === f.length) {
				out += f[p];
				continue;
			}
			const c = f[++p];
			if (c === '%') {
				out += '%';
				continue;
			}
			if (i >= args.length || !'sdifjoOc'.includes(c)) {
				out += '%' + c;
				continue;
			}
			const a = args[i++];
			switch (c) {
			case 's':
				out += typeof a === 'string' ? a :
				       typeof a === 'bigint' ? a + 'n' :
				       typeof a === 'object' && a !== null ? inspect(a, true, 0) :
				       formatPrimitive(a);
				break;
			case 'd':
			case 'i':
				out += typeof a === 'bigint' ? a + 'n' :
				       formatPrimitive(c === 'i' ? Math.trunc(Number(a)) : Number(a));
				break;
			case 'f':
				out += formatPrimitive(parseFloat(a));
				break;
			case 'j':
				out += JSON.stringify(a);
				break;
			case 'o':
			case 'O':
				out += inspect(a, true);
				break;
			case 'c':
				break;
			}
		}
		if (i < args.length)
			out += ' ';
	}
	for (let k = i; k < args.length; k++)
		out += (k > i ? ' ' : '') + inspect(args[k], false);
	return out;
}

const console = {
	log: (...a) => host.write(1, format(a) + '\n'),
	error: (...a) => host.write(2, format(a) + '\n'),
};
console.info = console.debug = console.log;
console.warn = console.trace = console.error;

/* Timers: a list, run by js.c once the script and its promise jobs are
   done - the same order of events as in a browser or Node, with one
   thread and no preemption */
const timers = [];
let timerSeq = 1;

function addTimer(fn, ms, args, repeat) {
	if (typeof fn !== 'function')
		throw new TypeError('The "callback" argument must be of type function');
	ms = Math.max(1, Number(ms) || 0);
	const t = { id: timerSeq++, fn, args, at: host.now() + ms,
	            every: repeat ? ms : 0 };
	timers.push(t);
	return t.id;
}

function clearTimer(id) {
	const i = timers.findIndex(t => t.id === id);
	if (i >= 0)
		timers.splice(i, 1);
}

global.console = console;
global.print = console.log;
global.setTimeout = (fn, ms, ...args) => addTimer(fn, ms, args, false);
global.setInterval = (fn, ms, ...args) => addTimer(fn, ms, args, true);
global.clearTimeout = global.clearInterval = clearTimer;
global.queueMicrotask = fn => { Promise.resolve().then(fn); };
global.performance = { now: () => host.now() };
global.readFile = host.readFile;
global.writeFile = host.writeFile;
global.exit = host.exit;

/* js.c's side of the event loop: how long until the next timer
   (-1 = none left), and running the ones that are due */
host.nextTimer = () => {
	if (!timers.length)
		return -1;
	let at = Infinity;
	for (const t of timers)
		at = Math.min(at, t.at);
	return Math.max(0, Math.ceil(at - host.now()));
};
host.fireTimer = () => {
	const now = host.now();
	let best = -1;
	for (let i = 0; i < timers.length; i++)
		if (timers[i].at <= now && (best < 0 || timers[i].at < timers[best].at))
			best = i;
	if (best < 0)
		return;
	const t = timers[best];
	if (t.every)
		t.at += t.every;
	else
		timers.splice(best, 1);
	t.fn(...t.args);
};
host.inspect = inspect;
host.format = format;
})
