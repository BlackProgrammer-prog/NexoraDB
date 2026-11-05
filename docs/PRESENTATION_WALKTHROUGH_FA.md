# راهنمای عملی ارائهٔ NexoraDB

این راهنما مکمل [دستورهای اجرا](../NEXORADB_PRESENTATION_COMMANDS.md) است: یک سناریوی شبکهٔ اجتماعی کوچک برای نشان‌دادن Document، رابطه، Index، Transaction، Graph، داشبورد React و اپ ترمینالی. نام زبان **NexoraQL** است. تمام دستورهای shell زیر داخل Ubuntu/WSL اجرا می‌شوند.

## ۱. آماده‌سازی و اجرای برنامه‌ها

بعد از نصب با pip، در محیط Python خود اجرا کنید:

```bash
nexoradb run
```

داشبورد React: `http://127.0.0.1:8000/admin/`. برای نسخهٔ نصب‌شده نیازی به npm یا سورس نیست.

در ترمینال دوم اپ ترمینالی را باز کنید:

```bash
nexoradb cli
```

در نسخهٔ دارای IAM بومی، ثبت مالک از صفحهٔ Register یا Query انجام نمی‌شود. پیش از اجرای سرور، برای دیتابیس خالی `build/debug/nexora_iam init /path/to/db` و برای دادهٔ موجود پس از پشتیبان‌گیری `build/debug/nexora_iam adopt /path/to/db` را اجرا کنید. نام مالک اولیه را خودتان انتخاب می‌کنید و الزاماً `root` نیست. سپس Login کنید. کاربران کالکشن `users` در مثال، دادهٔ اپلیکیشن هستند و حساب ورود به مدیریت نیستند.

برای ساخت حساب همراه رمز و مجوز بدون Query، در React مسیر `/security` یا در TUI تب **Account and access** را باز کنید. این مدیریت فقط برای SuperUser است و فقط مالک اولیه می‌تواند دیگران را SuperUser کند. سینتکس جدید نقش‌ها در ابتدای [social_demo.nql](../examples/presentation/social_demo.nql) قرار دارد؛ دستورهای دارای شناسهٔ نمونه را فقط پس از جایگزینی شناسه اجرا کنید. مراحل کامل [نقش‌ها، مدیریت کاربران و بازیابی رمز در شرایط مختلف](security/users-and-recovery-fa.md) را ببینید.

کوئری‌های مراحل بعد را در Query Editor داشبورد یا بخش Query اپ ترمینالی اجرا کنید. هر بلوک یک اجرای جداست؛ بلوک تراکنش باید کامل در یک اجرا ارسال شود. برای نمایش کالکشن‌های جدید ممکن است لازم باشد فهرست را refresh کنید.

## ۲. ساخت داده: کاربران، پست‌ها و دنبال‌کردن

دستورهای کامل در [فایل آمادهٔ NexoraQL](../examples/presentation/social_demo.nql) هم قرار دارند. برای ارائه، آن فایل را مرحله‌به‌مرحله اجرا کنید تا نتیجهٔ هر قابلیت دیده شود. نام‌ها باید در دیتابیس خالی باشند؛ اجرای دوبارهٔ کل فایل روی همان دیتابیس خطای نام کالکشن تکراری می‌دهد.

```sql
CREATE COLLECTION users (
  username STRING REQUIRED,
  age INT64,
  city STRING,
  tags ARRAY
) STRICT;

CREATE COLLECTION posts (
  author STRING REQUIRED,
  title STRING REQUIRED,
  likes INT64 DEFAULT 0
) STRICT;

CREATE COLLECTION follows (
  from_user STRING REQUIRED,
  to_user STRING REQUIRED
) STRICT;

INSERT INTO users VALUES ({"username":"ali","age":28,"city":"Tehran","tags":["db"]});
INSERT INTO users VALUES ({"username":"sara","age":25,"city":"Shiraz","tags":["graph"]});
INSERT INTO users VALUES ({"username":"reza","age":31,"city":"Tehran","tags":[]});
INSERT INTO users VALUES ({"username":"mina","age":22,"city":"Tabriz","tags":[]});

INSERT INTO posts VALUES ({"author":"ali","title":"Learning NexoraDB","likes":3});
INSERT INTO posts VALUES ({"author":"sara","title":"Graphs are useful","likes":5});

INSERT INTO follows VALUES ({"from_user":"ali","to_user":"sara"});
INSERT INTO follows VALUES ({"from_user":"ali","to_user":"reza"});
INSERT INTO follows VALUES ({"from_user":"sara","to_user":"reza"});
INSERT INTO follows VALUES ({"from_user":"reza","to_user":"mina"});
SHOW COLLECTIONS;
```

چهار کاربر، دو پست و چهار رابطه داریم. `_id` همهٔ اسناد خودکار ساخته می‌شود. در این مثال، ارتباط‌ها با نام کاربری یکتا تعریف می‌شوند؛ `username` شناسهٔ داخلی سند نیست. برای سادگی ارائه، نام کاربری را تغییر ندهید. `author` نویسندهٔ پست است؛ `from_user` و `to_user` جهت دنبال‌کردن را تعیین می‌کنند.

## ۳. خواندن، فیلتر، صفحه‌بندی و رابطهٔ بین کالکشن‌ها

```sql
SELECT * FROM users;
SELECT username, age FROM users WHERE age >= 25 LIMIT 10;
SELECT * FROM users LIMIT 2 SKIP 2;
COUNT FROM users WHERE city = 'Tehran';
EXISTS IN users WHERE username='ali';
SELECT * FROM posts LOOKUP JOIN users ON posts.author = users.username LIMIT 10;
```

نتیجهٔ شمارش تهران `2` و وجود `ali` برابر true است. JOIN هر پست را با اولین کاربر مطابق پیوند می‌دهد؛ خروجی پیوند در `__joined__` دیده می‌شود. این JOIN محدود است و معادل همهٔ انواع JOIN در SQL نیست. بدون ORDER BY ترتیب خروجی را تضمین‌شده معرفی نکنید؛ گرامر فعلی ORDER BY ندارد.

## ۴. ایندکس و قید رابطه

```sql
CREATE UNIQUE INDEX idx_users_username ON users (username);
CREATE INDEX idx_users_city ON users (city);
CREATE INDEX idx_posts_author ON posts (author);
SHOW INDEXES ON users;
ADD FOREIGN KEY fk_posts_author ON posts (author) REFERENCES users (username);
SHOW FOREIGN KEYS ON posts;
SELECT * FROM users WHERE username = 'ali';
```

ایندکس unique جلوی تکرار username را می‌گیرد. FK برای اعتبار نام کاربری نویسنده است؛ تعریف آن گراف ایجاد نمی‌کند. سرعت بیشتر روی چهار سند قابل اثبات نیست؛ برای مقایسهٔ کارایی باید workload و حجم مناسب اندازه‌گیری شود.

این خطاهای عمدی را **جداگانه** اجرا کنید، نه در فایل اصلی:

```sql
INSERT INTO users VALUES ({"username":"ali"});
```

```sql
INSERT INTO posts VALUES ({"author":"missing_user","title":"Invalid"});
```

انتظار: اولی نقض unique و دومی نقض FK. هر دو را پیش از ارائه روی باینری مورد استفاده بررسی کنید.

## ۵. ویرایش و حذف

```sql
UPDATE users SET city='Karaj' ADD TO SET tags='presenter' WHERE username='ali';
UPDATE posts INCREMENT likes BY 1 WHERE title='Learning NexoraDB';
SELECT * FROM users WHERE username='ali';
SELECT * FROM posts WHERE title='Learning NexoraDB';
INSERT INTO posts VALUES ({"author":"ali","title":"Temporary"});
DELETE FROM posts WHERE title='Temporary';
EXISTS IN posts WHERE title='Temporary';
```

لایک پست اول `4` می‌شود و وجود پست موقت در پایان false است.

## ۶. تراکنش: Commit و Rollback

در کد فعلی، هر Executor که GraphManager داشته باشد نوشتن تراکنشی را رد می‌کند، حتی قبل از ساخت mapping. بنابراین این مرحله را در Query Editor سرویس دارای گراف اجرا نکنید؛ اسکریپت مستقل زیر یک دیتابیس موقت بدون GraphManager می‌سازد و Commit/Rollback را با نتیجهٔ واقعی نشان می‌دهد: 

```bash
cd ~/NexoraDB
PYTHONPATH="$PWD/NexoraDB/src" NexoraDB/.venv/bin/python examples/presentation/transactions_demo.py
```

کوئری‌های زیر شرح همان دمو هستند و روی دیتابیس موقت اجرا می‌شوند. هر BEGIN تا پایان COMMIT/ROLLBACK باید در یک درخواست HTTP باشد؛ اجرای هر خط با دکمهٔ جدا صحیح نیست. اسکن و تغییرات bulk در تراکنش پشتیبانی نمی‌شوند.

```sql
BEGIN TRANSACTION;
INSERT INTO posts VALUES ({"author":"ali","title":"Committed"});
COMMIT;
```

```sql
EXISTS IN posts WHERE title='Committed';
```

انتظار true. سپس:

```sql
BEGIN TRANSACTION;
INSERT INTO posts VALUES ({"author":"sara","title":"Rolled back"});
ROLLBACK;
```

```sql
EXISTS IN posts WHERE title='Rolled back';
```

انتظار false. خطای دستور بعدی در یک دستهٔ معمولی، دستورهای autocommit قبلی را برنمی‌گرداند؛ چند دستور به‌تنهایی تراکنش نیستند.

## ۷. ساخت گراف روی داده‌های موجود

```sql
CREATE LIVE GRAPH social HETEROGENEOUS DIRECTED;
USE GRAPH social;
MAP NODE User FROM users KEY username;
MAP EDGE FOLLOWS FROM follows SOURCE from_user AS User TARGET to_user AS User;
BUILD GRAPH social;
SHOW GRAPHS;
GRAPH STATUS social;
GRAPH STATS social;
RENDER GRAPH social;
```

انتظار چهار گره و چهار یال. در بخش Graph داشبورد، گراف `social` را باز کنید و جهت یال‌ها را توضیح دهید. گراف یک projection از اسناد است؛ ذخیرهٔ سند و به‌روزرسانی گراف یک commit اتمیک مشترک ندارند و reconciliation خودکار کامل نیست.

## ۸. پرس‌وجوی رابطه و الگوریتم

برای هر درخواست جدید، `USE GRAPH social;` را همراه کوئری بفرستید تا به ماندگاری انتخاب گراف در درخواست قبلی وابسته نباشید.

```sql
USE GRAPH social;
SELECT * FROM users WHERE username='ali';
TRAVERSE User('ali') OUT FOLLOWS DEPTH 1 LIMIT 10;
EDGE EXISTS FROM User('ali') TO User('sara') TYPE FOLLOWS;
```

همسایه‌های مستقیم علی `sara` و `reza` هستند و وجود یال true است.

```sql
RUN LOCK MutualFriends ON social WITH user1='ali', user2='sara', edge_type='FOLLOWS';
RUN LOCK MostConnected ON social WITH metric='out', node_type='User' LIMIT 4;
```

دوست مشترک خروجی `reza` است؛ علی دو یال خروجی دارد. اجرای الگوریتم به runnerهای ثبت‌شده در build/سرویس وابسته است؛ این بخش را پیش از ارائه تمرین کنید.

نمایش اختیاری job پس‌زمینه:

```sql
RUN JOB ConnectedComponents ON social;
SHOW JOBS ON social;
```

شناسهٔ واقعی بازگشتی را به‌جای `job_id` بگذارید:

```sql
JOB STATUS 'job_id';
JOB RESULT 'job_id';
```

ممکن است job هنوز تمام نشده باشد؛ پس از پایان دوباره نتیجه بگیرید.

## ۹. Schema و مقدار پیش‌فرض

```sql
CREATE COLLECTION demo_settings (name STRING REQUIRED, enabled BOOL DEFAULT true) STRICT;
INSERT INTO demo_settings VALUES ({"name":"notifications"});
DESCRIBE COLLECTION demo_settings;
SELECT * FROM demo_settings;
```

انتظار `enabled=true`. نمونهٔ خطای جداگانه برای فیلد ناشناخته:

```sql
INSERT INTO demo_settings VALUES ({"name":"bad","unknown":1});
```

## ۱۰. پایش و نمایش دو رابط

```sql
SYSTEM STATUS;
SHOW PERFORMANCE;
SHOW INDEX ADVICE;
```

در React: Collections → کاربران و JSON یک سند → Query Editor → Indexها → Graph → Monitoring. نام دقیق تب‌ها را با نسخهٔ اجراشده تطبیق دهید.

در اپ ترمینالی: ورود به همان سرور → نمایش کالکشن‌ها → اجرای `SELECT * FROM users;` در بخش Query → اجرای کوئری گراف همراه USE GRAPH. نشان دهید هر دو رابط همان دادهٔ سرور را می‌بینند.

زمان پیشنهادی: معرفی و رابط‌ها ۳ دقیقه؛ CRUD و JOIN چهار دقیقه؛ Index و FK سه دقیقه؛ تراکنش سه دقیقه؛ گراف و الگوریتم پنج دقیقه؛ Schema و پایش دو دقیقه.

## ۱۱. نقشهٔ مستندات و اسکریپت‌ها

| نیاز | مرجع |
|---|---|
| نصب، معماری و شروع سریع | [README](../README.md) |
| اجرا، سرویس، log، reload، dev، dashboard، CLI و rebuild محلی | [دستورهای ارائه](../NEXORADB_PRESENTATION_COMMANDS.md) |
| محدودیت‌ها، پارامترها، JOIN و تراکنش | [قرارداد NexoraQL](cpp/NEXORAQL_CONTRACT.md) |
| دستورهای خط فرمان کاربر و توسعه‌دهنده | [CLI README](../NexoraDB/src/nexoradb/cli/README.md) |
| build، تست و benchmark | [راهنمای build](cpp/BUILD_TEST_BENCHMARK.md) |
| عملیات و بازیابی | [Operations](cpp/OPERATIONS_RUNBOOK.md)، [Recovery](cpp/PRODUCTION_RECOVERY.md) |
| الگوریتم‌ها | [راهنمای الگوریتم](../graph/algorithms/ALGORITHM_TEAM_GUIDE.md) |
| تست‌های نمونهٔ کوئری | [test_parser_queries.py](../tests/test_parser_queries.py) |
| اسکریپت‌های مهندسی، benchmark و اعتبارسنجی | [scripts/](../scripts/) |
| آزمایش‌های مقیاس بزرگ | [Scale Lab](../nexora-scale-lab/README.md) |

برای دیدن گزینه‌های واقعی نسخهٔ نصب‌شده، `nexoradb --help` و `nexoradb run --help` را اجرا کنید. پوشهٔ scripts ابزارهای مهندسی هم دارد؛ برای استفادهٔ روزمره لازم نیست تمام benchmarkها اجرا شوند.

برای نمایش فهرست تمام فایل‌های اسکریپتی مخزن:

```bash
rg --files scripts tests NexoraDB/examples nexora-scale-lab
```

این راهنما سناریوی ارائه و نقشهٔ منابع است؛ مرجع کامل تمام گزینه‌های تک‌تک ابزارهای پژوهشی نیست.


