// What rests on ICU, and the collector: what JavaScriptCore on ThreadOS
// prints here has to be exactly what Node.js prints for the same file
// (scripts/tests/t_jsc.py). Both carry ICU 78, so even the formats of
// dates and numbers in English and Polish have to come out the same.
const out = typeof print === 'function' ? print : console.log;

// Unicode properties and case, from ICU's tables
out(/\p{Lu}+/u.exec('abcĄĘÓxyz')[0], /\p{Script=Greek}/u.test('α'),
    /^\p{Emoji_Presentation}$/u.test('😀'), /\P{L}/u.test('Ż'));
out('ǅ'.toLowerCase(), 'ß'.toUpperCase(), 'ﬃ'.toUpperCase(),
    'İ'.toLocaleLowerCase('tr').length, 'I'.toLocaleLowerCase('tr'));
out('ŻÓŁW'.toLocaleLowerCase('pl'), 'gęś'.toLocaleUpperCase('pl'));

// Normalization
out('Å'.normalize('NFD').length, 'é'.normalize('NFC') === 'é',
    'ﬁ'.normalize('NFKC'), 'ẛ̣'.normalize('NFKD').length);

// Collation: Polish puts ą after a and ż after z
out(['ż', 'z', 'ą', 'a', 'b', 'ł', 'l', 'ź'].sort(new Intl.Collator('pl').compare).join(' '));
out(['b', 'A', 'a', 'B', 'ä'].sort(new Intl.Collator('en').compare).join(' '));
out('a'.localeCompare('B', 'en'), 'ą'.localeCompare('b', 'pl'),
    'résumé'.localeCompare('resume', 'en', { sensitivity: 'base' }));

// Numbers
out(new Intl.NumberFormat('en-US').format(1234567.891),
    new Intl.NumberFormat('pl-PL').format(1234567.891));
out(new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD' }).format(1234.5),
    new Intl.NumberFormat('pl-PL', { style: 'currency', currency: 'PLN' }).format(1234.5));
out(new Intl.NumberFormat('en', { style: 'percent' }).format(0.256),
    new Intl.NumberFormat('en', { notation: 'compact' }).format(1234567),
    new Intl.NumberFormat('pl', { notation: 'compact' }).format(1234567));
out((1234.5).toLocaleString('pl-PL'), (1234.5).toLocaleString('en-US'),
    (0.000123).toLocaleString('en', { maximumSignificantDigits: 2 }));
out(['pl', 'en'].map(l => [1, 2, 5, 22, 112].map(n => new Intl.PluralRules(l).select(n)).join(',')).join(' / '));
out(new Intl.PluralRules('en', { type: 'ordinal' }).select(2),
    new Intl.PluralRules('en', { type: 'ordinal' }).select(13));

// Dates and time zones
const d = new Date(Date.UTC(2024, 0, 2, 15, 4, 5));
out(d.toISOString(), d.getUTCDay(), JSON.stringify(d));
out(new Intl.DateTimeFormat('en-US', { dateStyle: 'long', timeZone: 'UTC' }).format(d));
out(new Intl.DateTimeFormat('pl-PL', { dateStyle: 'full', timeZone: 'UTC' }).format(d));
out(new Intl.DateTimeFormat('en-US', { timeZone: 'Europe/Warsaw', hour: '2-digit',
    minute: '2-digit', hourCycle: 'h23' }).format(d));
out(new Intl.DateTimeFormat('en-US', { timeZone: 'America/New_York', timeZoneName: 'short',
    hour: 'numeric' }).format(new Date(Date.UTC(2024, 6, 1, 12))));
out(new Intl.DateTimeFormat('pl', { month: 'long', timeZone: 'UTC' }).format(d),
    new Intl.DateTimeFormat('pl', { weekday: 'long', timeZone: 'UTC' }).format(d));
out(new Intl.RelativeTimeFormat('en').format(-1, 'day'),
    new Intl.RelativeTimeFormat('pl').format(3, 'hour'),
    new Intl.RelativeTimeFormat('en', { numeric: 'auto' }).format(1, 'day'));

// Lists, words, locales
out(new Intl.ListFormat('en', { type: 'conjunction' }).format(['a', 'b', 'c']),
    new Intl.ListFormat('pl', { type: 'disjunction' }).format(['a', 'b', 'c']));
const seg = new Intl.Segmenter('en', { granularity: 'word' });
out([...seg.segment('Hello, wide world!')].filter(s => s.isWordLike).map(s => s.segment).join('|'));
out([...new Intl.Segmenter('pl', { granularity: 'grapheme' }).segment('ż👍🏽é')].length);
out(Intl.getCanonicalLocales('EN-us')[0], new Intl.Locale('pl-PL').language,
    new Intl.Locale('pl').maximize().toString());
out(new Intl.DisplayNames('en', { type: 'region' }).of('PL'),
    new Intl.DisplayNames('pl', { type: 'language' }).of('en'));

// The collector: objects made and dropped until it has run many times
// over, with some kept - the kept ones must come through intact
const keep = [];
let sum = 0;
for (let i = 0; i < 60000; i++) {
    const o = { i, s: 'item ' + i, a: [i, i * 2, { deep: i % 7 }] };
    if (i % 1000 === 0)
        keep.push(o);
    sum += o.a[1] + o.a[2].deep;
}
const m = new Map(keep.map(o => [o.s, o]));
out(keep.length, sum, m.get('item 37000').a[2].deep, keep.every((o, k) => o.i === k * 1000));
let text = '';
for (let i = 0; i < 3000; i++)
    text += String.fromCharCode(97 + i % 26);
out(text.length, text.slice(2590, 2600), [...text.matchAll(/xyz/g)].length);
