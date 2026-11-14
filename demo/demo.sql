-- Demo script for the shell. Run it from this folder:
--   ../build/apps/shell.exe --db demo_db --frames 4 --file demo.sql
-- To start over, delete the demo_db folder (CREATE TABLE fails if the table already exists).

-- 1. Create tables (types: INT and VARCHAR(n), n from 1 to 255; no NULLs)
CREATE TABLE Emp (eid INT, did INT, name VARCHAR(20), age INT, salary INT);
CREATE TABLE Dept (did INT, dname VARCHAR(20));

-- 2. Fill them: from CSV (path in quotes) or row by row
LOAD Emp FROM 'emp.csv';
LOAD Dept FROM 'dept.csv';
INSERT INTO Dept VALUES (40, 'Legal');

-- 3. Gather statistics for the optimizer
ANALYZE Emp;
ANALYZE Dept;

-- 4. Change the memory budget (at least 3)
SET frames = 4;

-- 5. Queries
SELECT * FROM Emp WHERE Emp.age > 30;
SELECT e.name, d.dname FROM Emp e JOIN Dept d ON e.did = d.did;
SELECT e.name, d.dname FROM Emp e, Dept d WHERE e.did = d.did;
SELECT e.did, COUNT(*) AS n, AVG(e.salary) FROM Emp e GROUP BY e.did HAVING COUNT(*) > 1;
SELECT e.name, e.salary FROM Emp e ORDER BY e.salary DESC LIMIT 3;
SELECT DISTINCT e.did FROM Emp e;
SELECT e.did FROM Emp e UNION SELECT d.did FROM Dept d;
SELECT e.did FROM Emp e UNION ALL SELECT d.did FROM Dept d;
SELECT d.did FROM Dept d EXCEPT SELECT e.did FROM Emp e;
SELECT t.did, t.n FROM (SELECT e.did, COUNT(*) AS n FROM Emp e GROUP BY e.did) AS t WHERE t.n > 1;

-- 6. See the plan without running it
EXPLAIN SELECT e.name, d.dname FROM Emp e JOIN Dept d ON e.did = d.did;

-- 7. Run it and see estimated vs actual rows and I/O per plan node
EXPLAIN ANALYZE SELECT e.name, d.dname FROM Emp e JOIN Dept d ON e.did = d.did;
