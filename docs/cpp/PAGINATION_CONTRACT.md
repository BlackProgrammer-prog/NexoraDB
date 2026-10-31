# قرارداد pagination و stream — COR-001

تاریخ پذیرش فنی: ۲۰۲۶-۱۰-۲۸. این قرارداد برای `DocEngine::FindPage`، bindingهای
`find_page` / `iter_documents` / `next_ndjson_batch` و اسکن adapterهای lab است.
منبع ثابت‌های native: `core/PaginationLimits.h`؛ همان ثابت‌ها در ماژول Python با
نام‌های `PAGE_MAX_RECORDS`، `PAGE_MIN_BYTES`، `PAGE_DEFAULT_BYTES` و `PAGE_MAX_BYTES` صادر می‌شوند.

## حدود و معنای بایت

| پارامتر | قرارداد |
| --- | --- |
| تعداد در یک درخواست | ۱ تا ۱۰۰۰، شامل دو مرز؛ ۱۰۰۱ و ۴۰۹۶ نامعتبرند |
| بودجهٔ بایت | ۲ تا ۶۴ MiB؛ پیش‌فرض ۴ MiB؛ صفر به معنی پیش‌فرض نیست |
| معیار بایت | طول UTF-8 آرایهٔ JSON صفحه، شامل `[]` و کاماها؛ نه تعداد کاراکتر Python یا حجم BSON روی دیسک |
| صفحهٔ خالی موفق | `success=true`، دادهٔ `[]`، continuation خالی؛ raw stream معادل `(b'', 0)` |
| سندی که در بودجه جا نمی‌شود | خطای صریح؛ نه حذف سند، نه ارسال صفحهٔ بیش از سقف |

تعداد، سقف است نه تضمین اندازهٔ صفحه: محدودیت بایت می‌تواند صفحهٔ کوتاه‌تر ایجاد کند.
سقف ذخیرهٔ سند codec (۱۶ MiB) مستقل است؛ سند معتبر برای ذخیره الزاماً در صفحهٔ پیش‌فرض جا نمی‌شود.
پیش از ساخت snapshot یا مصرف cursor، درخواست خارج از حدود رد می‌شود؛ اندازهٔ بزرگ به‌صورت پنهان clamp نمی‌شود.
در C++ و `find_page` خطا در `PageResult.success/error_msg` است؛ **دادهٔ `[]` به‌تنهایی دلیل موفقیت نیست**.
کارخانهٔ stream در Python برای حدود نامعتبر `ValueError` و خطای پیمایش را `RuntimeError` می‌دهد.

## NDJSON و ownership

raw stream هر سند را در `{"document":<document>}\n` قرار می‌دهد. بودجه همچنان برای
آرایهٔ داخلی JSON است، نه envelope خروجی. برای batch غیرخالی با `n` سند، طول خروجی
حداکثر `max_page_bytes + 13*n - 1` بایت است. محدودیت wire مستقل را مصرف‌کننده باید با لحاظ
این سربار تنظیم کند. nested data، escapeها و Unicode حفظ می‌شوند.
مصرف object و raw روی یک stream قابل ترکیب نیست. از context manager یا `close()` استفاده شود؛
بستن دوباره مجاز است. برای API صفحهٔ مستقیم، `close_page(token)` نیز در Python در دسترس است.

درخواست با پارامتر نامعتبر continuation معتبر را مصرف نمی‌کند. خطای سند بزرگ در حین پیمایش،
session آن اسکن را آزاد می‌کند؛ برای retry با بودجهٔ بزرگ‌تر، **اسکن را از ابتدا** شروع کنید،
نه با token خطادار. در صورت توقف اختیاری صفحهٔ مستقیم، token را صریح ببندید.
سیاست عمر مطلق و expiry اسکن طولانی در [قرارداد COR-002](SNAPSHOT_LEASE_CONTRACT.md) تعریف شده است.

## سازگاری و lab

- `find_page(collection, condition=Condition(), limit=100, continuation_token='', max_page_bytes=4*1024**2)`
  اکنون بودجه را صریح می‌پذیرد؛ نام پارامتر C++ همان `max_bytes` است.
- streamهایی که قبلاً ۱۰۰۱..۴۰۹۶ را می‌پذیرفتند اکنون بلافاصله خطا می‌دهند؛ caller باید حداکثر ۱۰۰۰ درخواست کند.
- در C++، `max_bytes=0` قبلی دیگر sentinel نیست؛ آرگومان را حذف کنید یا مقدار پیش‌فرض بدهید.
- lab نوع integer و حدود را پیش از cursor بررسی می‌کند. عدد batch بارگذاری (مثلاً ۱۰۰۰۰)
  با اندازهٔ صفحه یکی نیست: caller اعتبارسنجی کامل، اندازهٔ scan را صریح تا ۱۰۰۰ محدود می‌کند.
- Nexora adapter ثابت‌های ماژول بارگذاری‌شده را بررسی می‌کند؛ binary قدیمی باید rebuild شود.
- PostgreSQL/Mongo adapter سقف تعداد مشترک دارند؛ بودجهٔ JSON native را به پروتکل شبکهٔ آن‌ها تعمیم نمی‌دهیم.

## شواهد پذیرش

- `tests/cpp/test_doc_engine.cpp`: مرزهای تعداد و بایت، exact bytes، صفحهٔ خالی، سند بزرگ،
  حفظ token پس از ورودی نامعتبر و اسکن ۲۵۰۳ سند بدون تکرار/افتادگی؛ مجموعهٔ کامل native: **۶۱ تست موفق**.
- `tests/test_pagination_contract.py`: **۵ تست واقعی native موفق**؛ object و NDJSON، Unicode/nested،
  صفحات ۱/۱۷/۱۰۰۰، درخواست ۱۰۰۱/۴۰۹۶، سند بیش از ۴ MiB و retry با ۸ MiB.
- `nexora-scale-lab/tests/test_validation_page_bounds.py`: **۳ تست موفق**؛ اسکن معتبر و جداسازی batch بارگذاری.
- مجموعهٔ lab: **۵۹ تست، ۷ skip مربوط به integration اختیاری**؛ این skipها شاهد اجرای Mongo/PostgreSQL نیستند.

فرمان‌های تکرار، از root پروژه در WSL، بعد از rebuild هسته و نصب/copy همان extension در package:

```bash
cmake --build build/release --target nexoradb_py nexora_doc_tests -j2
cmake -E copy_if_different nexoradb.cpython-310-x86_64-linux-gnu.so NexoraDB/src/nexoradb/native/nexoradb.cpython-310-x86_64-linux-gnu.so
build/release/tests/cpp/nexora_doc_tests
build/release-0.4.1-install-test/bin/python -m unittest discover -s tests -p test_pagination_contract.py -v
PYTHONPATH=nexora-scale-lab nexora-scale-lab/.venv/bin/python -m unittest discover -s nexora-scale-lab/tests -v
```

این‌ها تست‌های کوتاه صحت‌اند؛ تست بزرگ یا شاهد افزایش throughput نیستند. commit پایه و dirty worktree
را با `git status`/`git diff` همراه تحویل ثبت کنید؛ هیچ نتیجهٔ قدیمی به binary جدید نسبت داده نشده است.
