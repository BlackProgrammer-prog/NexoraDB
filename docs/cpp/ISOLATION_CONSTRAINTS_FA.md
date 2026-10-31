# COR-005 — isolation، history و قرارداد constraints

۲۰۲۶-۱۰-۳۰؛ دامنه: document engine، store اولیهٔ معتبر، یک فرایند مالک RocksDB و API عمومی native.
این قرارداد دربارهٔ graph، replication، تراکنش چند store، power-loss یا هر API خام RocksDB ادعا نمی‌کند.

## سطح واقعی

**پیش‌فرض explicit transaction: Read Committed با read-your-writes؛ نه Snapshot Isolation و نه Serializable.**

- `find_by_id_tx` عادی، آخرین مقدار committed را در همان call به‌اضافهٔ writeهای همان transaction می‌بیند. dirty read مجاز نیست.
- BeginTransaction به‌خودی‌خود snapshot سراسری برای خواندن نمی‌گیرد؛ خواندن دوباره می‌تواند مقدار تازه ببیند.
- mutation، سند/unique owner/counter را با قفل pessimistic می‌خواند و writeها تا commit منتشر نمی‌شوند.
- RYW فقط با APIهای Tx همان handle است. `find_by_id`، `find_many`، Count و SYSTEM INFO بیرون آن transaction هستند؛ حتی اگر caller یک handle باز داشته باشد.
- predicate/range read داخل explicit transaction، API عمومی پشتیبانی‌شده ندارد؛ وجود handle، query عادی را transactional نمی‌کند.
- pagination/stream هرکدام snapshot مستقل طبق [قرارداد pagination](PAGINATION_CONTRACT.md) و [lease](SNAPSHOT_LEASE_CONTRACT.md) دارند؛ این snapshot همان snapshot تراکنش نیست.
  صفحهٔ ادامه insert تازه را نمی‌بیند؛ scan تازه آن را می‌بیند. این تضمین برای یک cursor است، نه چند query مستقل.

### رفتار anomalyها

| حالت | پیش‌فرض | روش صحیح کاربرد |
| --- | --- | --- |
| dirty read | ممنوع؛ تست interleaving دارد | API Tx همان handle |
| non-repeatable read | مجاز | locking point read برای کلید مشخص |
| stale read-modify-write / lost application increment | ممکن؛ write سند atomic است ولی تصمیم caller اعتبارسنجی مجدد نمی‌شود | `$inc`/UpdateSpec.inc یا خواندن `for_update=True` و retry کل transaction |
| write skew روی invariant دلخواه چند سند | ممکن | تمام point keyهای تصمیم را پیش از تصمیم، به ترتیب ثابت قفل کن؛ range/predicate invariant هنوز پشتیبانی نمی‌شود |
| phantom بین queryهای مستقل | مجاز؛ query عمومی به Tx متصل نیست | یک snapshot cursor برای scan ثابت؛ برای predicate transaction ادعای Serializable نکن |
| duplicate unique عددی | ممنوع در فرمت v3 | conflict/timeout را مدیریت کن؛ alias int/double/±zero یک مقدار است |
| child insert در رقابت با parent delete | RESTRICT، بدون orphan جدید از store معتبر | constraint guard و revalidation زیر همان transaction |

## locking point read

```python
tx = engine.begin_transaction()
try:
    row = engine.find_by_id_tx(tx, 'items', 'a', for_update=True)
    if not row.success:
        raise RuntimeError(row.error_msg)
    # تصمیم و update_by_id_tx بر اساس همین مقدار، روی همین handle.
    result = engine.commit_transaction(tx)
    if not result.success:
        raise RuntimeError(result.error_msg)
finally:
    if tx.is_valid():
        engine.rollback_transaction(tx)
```

کلید موجود و ناموجود با `GetForUpdate` قفل می‌شود. قفل تا پایان transaction نگه داشته می‌شود؛ `for_update=False` پیش‌فرض قبلی است.
گرفتن همهٔ point lockها مانع phantom برای دامنهٔ کلیدهای ناشناخته نیست.
handle را بین موتورهای مختلف یا هم‌زمان بین threadها به اشتراک نگذار؛ این الگوی استفاده پشتیبانی/تأیید نشده است.

## قرارداد conflict/deadlock/retry

- پیش‌فرض‌ها: timeout قفل ۱۰۰۰ms، expiration تراکنش ۳۰۰۰۰ms، deadlock detection فعال با depth برابر ۵۰؛ قابل تنظیم با TransactionSettings.
- کدهای خطا: `transaction_timeout`، `transaction_deadlock` و `transaction_conflict`؛ پیام RocksDB نیز حفظ می‌شود.
- در خطای یک statement، savepoint عملیات mutation مانع باقی‌ماندن mutation ناقص آن می‌شود؛ handle ممکن است هنوز valid باشد.
  قرارداد کاربرد: **کل transaction را rollback کن، دوباره begin/read/decide/write کن**؛ فقط آخرین statement یا یک تصمیم قدیمی را retry نکن.
- retry محدود، deadline و backoff+jitter در caller لازم است؛ موتور retry نامحدود یا replay پنهانی ندارد.
- commit/rollback نهایی handle را invalid می‌کند. خطای IO/commit دارای outcome بالقوهٔ نامعلوم است؛ retry کورکورانه مجاز نیست؛ با ID درخواست/بازخوانی reconcile کن.
- unique/schema/FK validation error خطای موقتیِ قفل نیست؛ retry خودکار همان داده راه‌حل نیست.
- ترتیب ثابت گرفتن کلیدها احتمال deadlock را کم می‌کند، ولی جایگزین مدیریت deadlock/timeout نیست.

## FK: راهکار صحت‌محور فعلی و هزینهٔ آن

فقط store دارای FK یک کلید قفل تراکنشی مشترک (`meta:lock:fk-constraints`) برای mutationها می‌گیرد.
خواندن parent زیر این guard پیش از staging دوباره اعتبارسنجی می‌شود؛ parent delete و تغییر referenced value، childها را از view همان transaction بررسی می‌کنند.
حذف child و سپس parent در همان transaction مجاز است؛ rollback هر دو را برمی‌گرداند. self-reference حذف‌شونده همراه همان سند مانع حذف خودش نمی‌شود.
constraintها immediate هستند؛ deferred constraint، cascade و تغییر اتمیک parent-key همراه همهٔ childها پشتیبانی نمی‌شود.

این guard **store-wide** است و تا پایان explicit transaction نگه داشته می‌شود؛ write concurrency و تأخیر FKدار ممکن است افت کند.
RESTRICT با اسکن child اجرا می‌شود، نه reverse index؛ در دادهٔ بزرگ می‌تواند گران باشد. در no-FK store این scan/guard انجام نمی‌شود.
پس از حذف آخرین FK، latch محافظه‌کارانه تا reopen باقی می‌ماند. مسیر exclusive BSON ingest در store دارای FK به mutation تراکنشی fallback می‌کند.

افزودن FK، تحت DDL gate تمام childهای موجود را audit می‌کند؛ پس ممکن است روی store بزرگ طولانی باشد.
حذف collection یا index پشتیبانِ referenced field، تا حذف صریح FK رد می‌شود؛ حتی اگر index جایگزین داشته باشد.
FK روی non-unique referenced index فعلاً پشتیبانی قبلی خود را دارد؛ RESTRICT محافظه‌کارانه می‌تواند حذف یک parent دارای همان مقدار مشترک را هم رد کند.
null/missing/مقدار scalar تهی طبق رفتار فعلی از validation نوشتن FK رد می‌شوند؛ `_id` با نمایش scalar و non-id با canonical index tuple مقایسه می‌شود.
reverse RESTRICT برای non-id محافظه‌کارانه است و ممکن است child با مقدار رشتهٔ تهی را نیز مانع حذف parent دارای همان مقدار بداند.
این رفتار، SQL FK استاندارد با referenced unique key اجباری نیست؛ همسان‌سازی بیشتر یک تغییر قرارداد مستقل است.

store قدیمی با orphan از قبل، صرفاً با startup پاک‌سازی نمی‌شود. این تغییر از **ایجاد نقض جدید از حالت معتبر** جلوگیری می‌کند؛ audit/migration قدیمی با backup و سیاست رفع تعارض کار جداست.
`integrity_check` موجود، oracle کامل FK تاریخی نیست. برای دادهٔ قدیمی از نبود خطای آن، صحت FK را نتیجه نگیر.

## checker و شواهد

`scripts/transaction_history.py` یک oracle مستقل برای history کوچک و completion-ordered از point read/write است:

```bash
python scripts/transaction_history.py --history history.json --isolation read-committed
python scripts/transaction_history.py --history history.json --isolation strict-serializable
```

ورودی شامل `initial` (map کلید به مقدار) و `events` با `tx` و `op` است؛ opها: begin/read/write/commit/abort.
read/write شامل key/value هستند؛ فقط writeهای staging موفق ثبت می‌شوند. رخدادهای مورد بررسی عمداً در تست‌ها زمان‌بندی و منتظر completion می‌شوند.
checker برای initial keyهای موجود و مقادیر scalar این corpus است؛ absent و null جدا مدل نشده‌اند.
بررسی serial order exhaustive حداکثر ۸ transaction committed است؛ checker عمومی Jepsen یا اثبات همهٔ interleavingها نیست.

`tests/test_isolation_contract.py`: dirty/RYW/abort، nonrepeatable، lost application update، write skew، point lock/retry، snapshot/new scan، unique conflict، deadlock/timeout، دو ترتیب child/parent، non-id parent update، DDL و audit دادهٔ قبلی؛ همچنین `_id` immutable.
historyهای مجاز Read Committed ولی غیرserializable عمداً ثبت و توسط oracle دوم رد می‌شوند؛ تست سبز آن‌ها ادعای نبود anomaly نیست.
مدل عملیات مستقل: [COR-006](REFERENCE_MODEL_FA.md).

شواهد محلی: ۱۸ تست اختصاصی isolation/constraints؛ مجموعهٔ ۱۲۹تایی Python مرتبط و ۷۳ C++ موفق.
۱۲ SIGKILL/reopen دوباره پاس شد: `build/cor005-crash-20261030/report.json`؛ نه آزمون قطع برق.
هیچ تست چندساعته، migration/حذف DB موجود، commit یا انتشار انجام نشده است.

## کارهای بعدی، خارج از پذیرش محدود این قرارداد

- [ ] reverse FK index نسخه‌دار و guardian lockهای ریزدانه با migration و interleaving معادل؛ سپس حذف guard سراسری با پروفایل مستند.
- [ ] transaction predicate read با قرارداد phantom/range locking یا OCC؛ بدون آن Serializable عمومی اعلام نشود.
- [ ] audit کامل FK تاریخی روی snapshot ثابت و گزارش bounded-memory، بدون حذف خودکار.
- [ ] کنترل مالکیت handle موتور/thread و lease منابع explicit transaction.
