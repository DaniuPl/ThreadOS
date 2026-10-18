// What only ThreadOS's js has: files on the ThreadOS disk, the script's
// arguments, an exit code of its own choosing
const text = 'Zażółć gęślą jaźń\nline two\n';
console.log('written', writeFile('pl.txt', text));
const back = readFile('pl.txt');
console.log('read back the same', back === text, back.length);
writeFile('data.json', JSON.stringify({ list: [1, 2, 3, 4] }));
console.log('json sum', JSON.parse(readFile('data.json')).list.reduce((a, b) => a + b));
try {
	readFile('nope.txt');
} catch (e) {
	console.log('missing:', e.name, e.message);
}
console.log('args', scriptArgs);
exit(4);
