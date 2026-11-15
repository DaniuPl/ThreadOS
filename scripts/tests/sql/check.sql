-- SQLite on ThreadOS, part two: a database opened again - written by
-- part one on ThreadOS, or by the host's SQLite and fetched over HTTP.
-- (list mode: the shell would draw boxes on ThreadOS's console, a tty)
.mode list
PRAGMA integrity_check;
SELECT count(*), sum(people) FROM city;
SELECT count(*), round(sum(value), 1), min(note), max(note) FROM reading;
SELECT name, people FROM city WHERE id IN (SELECT city FROM reading WHERE day = 500) ORDER BY name;
SELECT * FROM audit ORDER BY what;
SELECT title FROM doc WHERE doc MATCH 'browser OR disk' ORDER BY title;
SELECT name FROM sqlite_schema ORDER BY type, name;
