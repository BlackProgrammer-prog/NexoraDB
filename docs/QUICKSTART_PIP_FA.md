# شروع سریع پس از نصب با pip

دستورها را در Linux یا Ubuntu داخل WSL، با محیط Python نصب‌شدهٔ NexoraDB اجرا کنید.

## اجرای دیتابیس و داشبورد React

از نسخهٔ ۰.۵.۳، قبل از اجرای سرور باید مالک را آفلاین بسازید. برای دیتابیس خالی:

```bash
nexoradb-iam --version
nexoradb-iam init /absolute/path/to/nexoradb-data
export NEXORADB_DB_PATH=/absolute/path/to/nexoradb-data
```

برای دیتابیس موجود ابتدا سرویس را متوقف و پشتیبان تهیه کنید و به جای `init`،
`nexoradb-iam adopt /absolute/path/to/nexoradb-data` بزنید. نام مالک دلخواه است؛
هشت کد بازیابی را آفلاین نگه دارید. مسیر graph را نیز با `NEXORADB_GRAPH_DIR` مشخص کنید.

```bash
nexoradb run
```

داشبورد: http://127.0.0.1:8000/admin/

با نام و رمز مالک انتخاب‌شده وارد شوید؛ Register عمومی غیرفعال است. برای نسخهٔ نصب‌شده به npm یا سورس پروژه نیاز ندارید. مدیریت کاربران بدون Query در React مسیر `/security` و TUI بخش **Account and access** است. [نقش‌ها و بازیابی رمز در شرایط مختلف](security/users-and-recovery-fa.md) را ببینید.

## اجرای اپ ترمینالی

در ترمینال دوم:

```bash
nexoradb cli
```

با همان حساب وارد شوید. کوئری‌های زیر را در بخش Query داشبورد یا اپ ترمینالی اجرا کنید.

## کالکشن‌های ساختاریافته

```sql
CREATE COLLECTION users (
  username STRING REQUIRED,
  age INT64 REQUIRED
) STRICT;

CREATE COLLECTION follows (
  from_user STRING REQUIRED,
  to_user STRING REQUIRED
) STRICT;
```

`REQUIRED` یعنی فیلد اجباری است؛ `STRICT` فیلد تعریف‌نشده را رد می‌کند. `_id` شناسهٔ ویژهٔ سند است و خودکار ساخته می‌شود؛ نیازی به تعریف در اسکیما یا واردکردنش نیست. در این دمو رابطه‌ها بر اساس `username` یکتا هستند و کلید گراف هم `username` است.

## داده، ایندکس و خواندن

```sql
INSERT INTO users VALUES ({"username":"ali","age":28});
INSERT INTO users VALUES ({"username":"sara","age":25});
INSERT INTO follows VALUES ({"from_user":"ali","to_user":"sara"});

CREATE UNIQUE INDEX idx_username ON users (username);
SELECT * FROM users WHERE age >= 25;
DESCRIBE COLLECTION users;
```

## گراف روی همان داده‌ها

```sql
CREATE LIVE GRAPH social HETEROGENEOUS DIRECTED;
USE GRAPH social;
MAP NODE User FROM users KEY username;
MAP EDGE FOLLOWS FROM follows SOURCE from_user AS User TARGET to_user AS User;
BUILD GRAPH social;
RENDER GRAPH social;
```

در اجرای بعدی:

```sql
USE GRAPH social;
TRAVERSE User('ali') OUT FOLLOWS DEPTH 1 LIMIT 10;
```

خروجی پیمایش شامل `sara` است. ساخت کالکشن‌ها و داده‌ها را فقط یک بار انجام دهید.

## اجرای پس‌زمینه و توقف

پس از توقف اجرای عادی با Ctrl+C:

```bash
nexoradb run --service
nexoradb run --log
nexoradb run --stop
```

برای سناریوی کامل‌تر شامل JOIN، FK و محدودیت تراکنش، [راهنمای ارائه](PRESENTATION_WALKTHROUGH_FA.md) را ببینید.
