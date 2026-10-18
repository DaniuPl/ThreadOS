// The event loop: synchronous code first, then every promise job, then
// the timers in the order they fall due - the order Node.js keeps, and
// the one QuickJS on ThreadOS has to keep too
const log = [];
const t0 = performance.now();
setTimeout(() => log.push('timeout 300'), 300);
setTimeout(() => {
	log.push('timeout 0');
	Promise.resolve().then(() => log.push('microtask inside a timer'));
}, 0);
setTimeout(() => log.push('timeout 150'), 150);
Promise.resolve().then(() => log.push('then 1')).then(() => log.push('then 2'));
queueMicrotask(() => log.push('microtask'));
(async () => {
	log.push('async start');
	await null;
	log.push('after await');
	const v = await new Promise(r => setTimeout(() => r('resolved later'), 200));
	log.push(v);
	try {
		await Promise.reject(new TypeError('no'));
	} catch (e) {
		log.push('caught ' + e.name);
	}
})();
log.push('sync end');

let ticks = 0;
const iv = setInterval(() => {
	if (++ticks === 3) {
		clearInterval(iv);
		log.push('interval x3');
	}
}, 30);
const never = setTimeout(() => log.push('cancelled!'), 250);
clearTimeout(never);

Promise.all([1, Promise.resolve(2), new Promise(r => setTimeout(r, 50, 3))])
	.then(v => log.push('all ' + v.join(',')));
Promise.allSettled([Promise.reject(1), 2]).then(r => log.push(r.map(x => x.status).join(',')));
Promise.race([new Promise(r => setTimeout(r, 500, 'slow')), new Promise(r => setTimeout(r, 1, 'fast'))])
	.then(v => log.push('race ' + v));
Promise.any([Promise.reject(1), Promise.resolve('any ok')]).then(v => log.push(v));

async function* gen() { for (let i = 0; i < 3; i++) { await null; yield i; } }
(async () => { const got = []; for await (const x of gen()) got.push(x); log.push('for await ' + got); })();

setTimeout(() => {
	console.log(log.join('\n'));
	const ms = performance.now() - t0;
	console.log('waited at least the longest timer:', ms >= 300);
	console.log(Promise.resolve(42), new Promise(() => {}));
}, 400);
