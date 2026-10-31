# RES-002 — ماتریس مقایسهٔ نسخهٔ ۱

بازبینی مستندات: ۲۰۲۶-۱۰-۲۸. این **طرح مقایسه** است، نه ادعای نصب/اجرا یا آخرین نسخهٔ محصولات.
نسخه‌های تاریخی compose (`MongoDB 8.0.3` و `PostgreSQL 17.0`) صرفاً مبنای آزمایش‌های قبلی‌اند؛ برای اجرای جدید باید patch امنیتیِ پشتیبانی‌شده انتخاب و به digest قفل شود. آن نتایج بازنویسی نمی‌شوند.

## انتخاب نسخه و edition

| محصول | خانوادهٔ انتخابی | edition/deployment | قفل نسخهٔ اجرایی |
| --- | --- | --- | --- |
| NexoraDB | source فعلی + build واقعی | embedded؛ API در track جدا | commit و SHA فایل .so/executable، Release بدون sanitizer |
| MongoDB | 8.0.34 | Community، standalone WiredTiger | binary نصب‌شده همین نسخه را گزارش کرد؛ server runtime/image digest هنوز ثبت نشده |
| PostgreSQL | 17.11 | PostgreSQL community، single server | **هدف جدید**؛ binary محلی 14.24 است، نه 17.11؛ نصب/ارتقا انجام نشد |
| SQL Server | 2022 CU27+GDR، build 16.0.4305.5 | Developer، single server؛ فقط dev/test | هدف انتخاب‌شده از فهرست رسمی؛ نصب نشده |
| Oracle | 21.3 از خانوادهٔ 21c | XE، single server | baseline تاریخی/ایزوله؛ SHA فایل OL8 در config ثبت شد؛ نصب و بررسی اجازهٔ انتشار انجام نشده |
| Neo4j | 5.26.32 LTS | Community، single server؛ Cypher 5 | هدف انتخاب‌شده از release notes رسمی؛ نصب نشده |

برای pending هیچ نتیجهٔ رتبه‌بندی مجاز نیست. این انتخاب‌ها الزام به ارتقای سرویس‌های فعلی یا نصب خودکار ایجاد نمی‌کنند.
Developer نمایندهٔ محدودیت edition تجاری دیگر نیست؛ XE نمایندهٔ Enterprise نیست؛ Community نمایندهٔ Neo4j Enterprise نیست.

نسخهٔ ماشین‌خوان: [`research-products-v1.json`](../../nexora-scale-lab/config/research-products-v1.json).
این pin انتخاب پژوهشی است، نه ادعای «همه نصب‌اند» یا «همیشه جدیدترین هستند»؛ تغییر نسخه پس از نتیجه نیازمند طرح تازه است.
XE انتخاب امنیتی/production نیست؛ محدودیت patch و قدمت آن باید در هر نتیجه آشکار بماند و برای مقایسهٔ امنیتی عمومی استفاده نشود.

## قرارداد مقایسه

| محصول | مدل W1/W2 | ACK محلیِ درخواستی | isolation انتخابی و فاصله | transport / ایندکس |
| --- | --- | --- | --- | --- |
| Nexora | BSON و graph projection | `DocumentDurability.Durable`، WAL sync | یک سند/عملیات؛ تضمین snapshot ترکیبی سند+گراف اثبات نشده | embedded pybind؛ API جدا؛ primary ID + username |
| Mongo | BSON / edge collection + `$graphLookup` | `w:1,j:true` | readConcern=local، standalone؛ batch اتمیک چندسندی نیست | TCP، PyMongo، `_id` و username |
| PostgreSQL | JSONB + text ID / edge table + recursive CTE | fsync=on، synchronous_commit=on، full_page_writes=on | READ COMMITTED، هر statement snapshot؛ multi-query hydration snapshot مشترک مفروض نیست | TCP، psycopg، PK + expression index روی username |
| SQL Server | JSON text معتبر در nvarchar / node-edge tables یا CTE معادل | delayed durability=DISABLED | READ COMMITTED؛ RCSI effective ثبت شود، برای طرح هدف RCSI=OFF | TCP/TDS، driver/version ثبت؛ PK + indexed computed username |
| Oracle XE | JSON در مدل سازگار 21c / edge table + recursive query | commit synchronous WAIT؛ NOWAIT ممنوع | READ COMMITTED؛ statement-consistent، نه snapshot مشترک همهٔ statementها | TCP/Oracle Net، driver ثبت؛ PK + function-based username |
| Neo4j CE | properties محدود برای W2؛ BSON کامل N/A | commit عادی transaction، بدون async ACK سفارشی | READ COMMITTED؛ semantics graph query باید با oracle normalize شود | TCP/Bolt، driver ثبت؛ index شناسهٔ User |

W1 تمام fieldها ازجمله nested object/null/array دارد؛ تبدیل آن به property graph بدون قرارداد lossless مجاز نیست. در v1، Neo4j برای W1/W3 document-model **N/A** است؛ W2 adapter هنوز پیاده نشده است.
ایندکس username باید با EXPLAIN/profile و شمارش نتیجه بررسی شود؛ ایجاد index لزوماً اثبات استفاده نیست.
برای هر محصول limits حافظه و همهٔ processهای مرتبط ثبت شوند؛ page cache فقط یک بار در cgroup شمرده شود. W3 با Oracle XE باید علاوه بر RAM زیر سقف CPU/data همان edition بماند؛ مقایسهٔ Xe با ۴ CPU بدون افشای سقف داخلی معتبر نیست.

## منابع رسمی و حدود استناد

- نسخه‌های دقیق: [MongoDB 8.0 changelog](https://www.mongodb.com/docs/v8.0/release-notes/8.0-changelog/)، [PostgreSQL 17.11](https://www.postgresql.org/docs/release/17.11/)، [SQL Server update/build list](https://learn.microsoft.com/troubleshoot/sql/general/determine-version-edition-update-level)، [Neo4j 5.26.32](https://neo4j.com/release-notes/database/neo4j-5-26-32/)، [Oracle XE download و SHA رسمی](https://www.oracle.com/database/technologies/xe-downloads.html).
- [MongoDB 8.0 write concern](https://www.mongodb.com/docs/v8.0/reference/write-concern/): `j:true` انتظار journal است؛ جایگزین تضمین replication/failover نیست.
- [PostgreSQL 17 isolation](https://www.postgresql.org/docs/17/transaction-iso.html) و [WAL settings](https://www.postgresql.org/docs/17/runtime-config-wal.html): snapshot و دوام انتخابی باید جداگانه ثبت شوند.
- [SQL Server 2022 editions](https://learn.microsoft.com/en-us/sql/sql-server/editions-and-components-of-sql-server-2022?view=sql-server-ver16)، [durability](https://learn.microsoft.com/en-us/sql/relational-databases/logs/control-transaction-durability?view=sql-server-ver16)، [isolation](https://learn.microsoft.com/en-us/sql/t-sql/statements/set-transaction-isolation-level-transact-sql?view=sql-server-ver16).
- [Oracle 21c XE restrictions](https://docs.oracle.com/en/database/oracle/oracle-database/21/xeinl/licensing-restrictions.html): سقف دو core، RAM دو GiB و user data دوازده GB؛ این اعداد محدودیت edition هستند، نه الگوریتم هسته.
- [Oracle 21c COMMIT](https://docs.oracle.com/en/database/oracle/oracle-database/21/sqlrf/COMMIT.html): WAIT/NOWAIT باید ثبت شود.
- [Neo4j editions](https://neo4j.com/docs/operations-manual/current/introduction/) و [concurrent access](https://neo4j.com/docs/operations-manual/current/database-internals/concurrent-data-access/): منابع current هستند؛ قابلیت‌های جدیدتر را به 5.26 تعمیم ندهید. تطبیق با بستهٔ نصب‌شده پیش‌شرط run است.

ثبت publication restrictions و تأیید اجازهٔ انتشار benchmark برای نرم‌افزارهای اختصاصی پیش از اجرا/انتشار لازم است؛ این سند نظر حقوقی یا اجازهٔ license نیست.

## پیش‌شرط اجرای مقایسهٔ واقعی، خارج از تکمیل سند ماتریس

- [x] انتخاب version/edition دقیق برای هر پنج محصول با منبع؛ runtime/digestهای ناموجود به‌صراحت null و NOT INSTALLED هستند.
- [ ] ثبت خروجی واقعی server version/edition و pin digestهای هنوز ناموجود قبل از run.
- [ ] دریافت effective config و fail-closed اگر durability/resource profile برقرار نیست.
- [ ] adapter و oracle برای SQL Server، Oracle و Neo4j؛ در حال حاضر adapter واقعی lab فقط Nexora/Mongo/Postgres است.
- [ ] تایید EXPLAIN و semantics W2 و snapshot/collation برای هر adapter.
- [ ] بازبینی منابع version-specific؛ current docs به‌تنهایی شاهد قابلیت نسخهٔ قدیمی نیست.
