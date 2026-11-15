-- SQLite on ThreadOS, part one: build a database and ask it things.
-- The test runs this here and on the host with the same SQLite, and
-- the two outputs have to be identical.
-- (list mode: the shell would draw boxes on ThreadOS's console, a tty)
.mode list
PRAGMA page_size = 1024;
PRAGMA journal_mode = DELETE;
CREATE TABLE city(id INTEGER PRIMARY KEY, name TEXT NOT NULL, country TEXT,
                  people INTEGER, founded INTEGER);
INSERT INTO city(name, country, people, founded) VALUES
  ('Warsaw', 'PL', 1863056, 1300), ('Krakow', 'PL', 804237, 1257),
  ('Gdansk', 'PL', 486022, 997), ('Berlin', 'DE', 3878100, 1237),
  ('Hamburg', 'DE', 1964021, 808), ('Prague', 'CZ', 1384732, 880),
  ('Brno', 'CZ', 400566, 1243), ('Vienna', 'AT', 2005760, 1155);
SELECT country, count(*), sum(people), max(name) FROM city
  GROUP BY country ORDER BY sum(people) DESC;
SELECT name, people, rank() OVER (PARTITION BY country ORDER BY people DESC),
       round(100.0 * people / sum(people) OVER (), 2)
  FROM city ORDER BY country, people DESC;

-- Many rows, an index, and a join
CREATE TABLE reading(id INTEGER PRIMARY KEY, city INTEGER REFERENCES city(id),
                     day INTEGER, value REAL, note TEXT);
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000)
INSERT INTO reading(city, day, value, note)
  SELECT 1 + i % 8, i / 8, (i * 7919 % 1000) / 10.0,
         printf('r%05d-%s', i, hex(i * 31 % 256)) FROM n;
CREATE INDEX reading_city_day ON reading(city, day);
SELECT c.name, count(r.id), round(avg(r.value), 3), min(r.note), max(r.day)
  FROM city c JOIN reading r ON r.city = c.id GROUP BY c.id ORDER BY c.id;
SELECT count(*) FROM reading WHERE city = 3 AND day BETWEEN 100 AND 199;

-- A sort bigger than the cache, by several threads
PRAGMA cache_size = 20;
PRAGMA threads = 4;
SELECT sum(rowid * pos) % 1000000007 FROM (
  SELECT rowid, row_number() OVER (ORDER BY (id * 2654435761) % 1000003, id) AS pos
    FROM reading);
PRAGMA cache_size = 2000;

-- A transaction rolled back: the file grew, and is cut back
BEGIN;
INSERT INTO reading(city, day, value, note)
  SELECT city, day + 10000, value, note || '-x' FROM reading;
SELECT count(*) FROM reading;
ROLLBACK;
SELECT count(*), max(day) FROM reading;

-- Savepoints, a trigger, a view
CREATE TABLE audit(what TEXT);
CREATE TRIGGER city_grows AFTER UPDATE OF people ON city
  BEGIN INSERT INTO audit VALUES (old.name || ' ' || old.people || '->' || new.people); END;
SAVEPOINT a;
UPDATE city SET people = people + 1000 WHERE country = 'CZ';
SAVEPOINT b;
UPDATE city SET people = 0;
ROLLBACK TO b;
RELEASE a;
SELECT * FROM audit ORDER BY what;
CREATE VIEW big AS SELECT name FROM city WHERE people > 1500000;
SELECT group_concat(name, ',') FROM (SELECT name FROM big ORDER BY name);

-- Functions: text, numbers, dates, JSON
SELECT upper('threados'), length('zażółć'), substr('abcdef', 2, 3),
       replace('a-b-c', '-', '+'), instr('hello', 'll'), trim('  x  ');
SELECT 7 / 2, 7 / 2.0, 7 % 3, abs(-4.5), round(2.675, 2), 1e308 * 10,
       sqrt(2), pow(2, 10), ln(exp(1)), cast('12abc' AS INTEGER);
SELECT date('2024-02-29', '+1 year'), datetime('2026-11-15 12:00:00', '-90 minutes'),
       strftime('%j %W %w', '2026-11-15'), julianday('2000-01-01') - julianday('1970-01-01');
SELECT json_object('city', name, 'people', people) FROM city WHERE id = 1;
SELECT json_extract('{"a":[1,2,{"b":"c"}]}', '$.a[2].b'), json_array_length('[1,2,3]');
SELECT key, value FROM json_each('{"x":1,"y":[2,3],"z":null}');

-- Full-text search
CREATE VIRTUAL TABLE doc USING fts5(title, body);
INSERT INTO doc VALUES
  ('Threads', 'POSIX threads arrive in the C library with futexes'),
  ('Browser', 'the browser draws pages with litehtml and runs JavaScript'),
  ('Disk', 'TFS keeps files in blocks with direct and indirect pointers'),
  ('Locks', 'a futex wakes the threads that wait on a word');
SELECT title FROM doc WHERE doc MATCH 'threads' ORDER BY rank;
SELECT title, snippet(doc, 1, '[', ']', '...', 4) FROM doc WHERE doc MATCH 'futex*' ORDER BY title;

-- Smaller again: VACUUM rewrites and truncates the file
DELETE FROM reading WHERE id % 4 != 0;
VACUUM;
SELECT count(*), sum(value) FROM reading;
PRAGMA integrity_check;
