# COR-007 — ماتریس خطای ذخیره‌سازی و recovery

دامنهٔ پذیرش: موتور document، در اجرای کوچک و قطعی روی Linux/WSL؛ مرز سند، ایندکس، شمارنده، WAL و ACK.
پروفایل ماتریس تزریق `Durable` است. تست ورودی‌های COR-003 علاوه بر آن `Buffered` را در crash فرایند بررسی می‌کند.

## مسیرها و oracle

۹ مسیر مستقل: `InsertOne`، JSON batch، BSON batch، exclusive BSON ingest، update/delete نقطه‌ای،
`UpdateMany`، `DeleteMany` و transaction شامل insert+update+delete.
ingest با ایندکس معمولی و بدون schema/FK اجرا می‌شود تا مسیر exclusive واقعی به fallback تبدیل نشود.
یک تست جداگانه commit مشترک دو collection با unique و FK را نیز بررسی می‌کند.

oracle از دو وضعیت کامل و از پیش معلوم ساخته می‌شود؛ وضعیت مورد انتظار از خروجی موتور استخراج نمی‌شود.
پس از reopen، محتوای کامل اسناد، point read، full scan، page، query ایندکسی واقعی، نبود کلیدهای قدیمی،
COUNT و شمارندهٔ ذخیره‌شده و integrity check با آن مقایسه می‌شوند.
برای خطای sync، ابتدا عضویت در یکی از دو وضعیت کامل بررسی می‌شود؛ صرفاً سازگاری موتور با خروجی خودش کافی نیست.

## ماتریس failure و recovery

| تزریق/مرز | پوشش | پاسخ و نتیجهٔ مورد پذیرش |
| --- | --- | --- |
| پس از document/index/counter، پیش از commit | ۲۵ ترکیب مسیر/مرز | خطا؛ وضعیت قبلی در همان process و پس از reopen |
| ENOSPC در append WAL | هر ۹ مسیر | `io_error`، بدون ACK موفق، health و shutdown ناموفق؛ recovery وضعیت قبلی |
| EACCES در append WAL | هر ۹ مسیر | مانند ENOSPC |
| خطای sync/fsync WAL | هر ۹ مسیر | بدون ACK موفق؛ نتیجهٔ commit نامعلوم؛ recovery فقط تمامِ وضعیت قبلی یا تمامِ وضعیت جدید |
| خطای savepoint | ۸ ترکیب insert/update/delete و مرز staging | mutation ردشده حذف می‌شود؛ staging موفق قبلی با commit همان transaction حفظ می‌شود |
| unique و FK، دو collection در یک transaction | هر سه خطای WAL | parent و child با هم بازیابی می‌شوند یا هیچ‌کدام؛ شمارنده‌ها و unique index سازگار |
| SIGKILL در document/index/counter | همان ۲۵ ترکیب | recovery وضعیت قبلی؛ هیچ destructor/shutdown اجرا نمی‌شود |
| SIGKILL پیش از append WAL | هر ۹ مسیر | وضعیت قبلی |
| SIGKILL پس از append یا پیش از sync WAL | هر ۹ مسیر | تمامِ وضعیت قبلی یا تمامِ وضعیت جدید؛ ACK صادر نشده |
| SIGKILL پس از sync و پیش از ACK | هر ۹ مسیر | وضعیت جدید؛ ACK هنوز صادر نشده |
| SIGKILL بلافاصله پس از ACK | هر ۹ مسیر | وضعیت جدید کامل |
| نوشتن نصف buffer WAL، سپس خطای I/O و SIGKILL | هر ۹ مسیر | NACK و latch؛ reopen با `Corruption: truncated record body` رد می‌شود |
| تغییر payload WAL با CRC قبلی | یک store تازه پس از ACK | open با checksum mismatch رد می‌شود |
| حذف آخرین بایت WAL | یک store تازه پس از ACK | open با truncated record body رد می‌شود |
| تغییر data block در SST با CRC قبلی | یک store تازه پس از ACK | open با block checksum mismatch رد می‌شود |
| envelope ناشناخته، BSON ناقص، JSON خراب | سه payload مستقل | point/full scan/index/page و update/delete خطا می‌دهند؛ سند خراب skip نمی‌شود |
| entry ایندکس با owner گم‌شده | query، conditional COUNT و EXISTS | خطای `corruption`؛ نتیجهٔ خالی موفق مجاز نیست |
| counter نامعتبر، منفی، overflow یا دارای پسوند/علامت/فاصله | ۷ مقدار | COUNT خطا؛ size برابر sentinel خطای `-1`؛ insert بدون اثر |
| counter حذف‌شده | COUNT، insert، integrity | خطا؛ اسناد قبلی حفظ می‌شوند |
| counter عددی ولی ناسازگار با اسناد | integrity بدون بررسی ایندکس و delete گروهیِ دارای underflow | خطا؛ حذف نیمه‌کاره یا تبدیل خاموش مقدار به صفر مجاز نیست |

update شمارنده را تغییر نمی‌دهد؛ ترکیب update/counter تولید نمی‌شود و هیچ skip برای آن به‌عنوان تست موفق حساب نشده است.
خطاهای ENOSPC/EACCES در `rocksdb::Env`، روی append واقعی WAL تزریق می‌شوند؛ دیسک میزبان واقعاً پر یا permission آن تغییر داده نمی‌شود.
تست نوشتن ناقص، واقعاً بخشی از buffer را به فایل خصوصی تست می‌نویسد و سپس خطا برمی‌گرداند.

## اصلاحات حاصل از تست‌ها

1. ارجاع ایندکس به سند گم‌شده در MultiGet دیگر بی‌صدا `continue` نمی‌شود؛ query خطای corruption می‌دهد.
2. COUNT شمارندهٔ ناخوانا/گم‌شده را به صفر موفق تبدیل نمی‌کند؛ کل رشته و نامنفی‌بودن آن بررسی و corruption در health ثبت می‌شود.
3. خواندن size، پسوند عددی خراب را قبول نمی‌کند. mutation نیز counter منفی و underflow را رد می‌کند.
4. `MutationBuilder::Commit` خطای I/O/corruption را در نقطهٔ مشترک latch می‌کند؛ سلامت آن به wrapper ورودی وابسته نیست.
   پیش از اصلاح، ENOSPC/EACCES در UpdateMany/DeleteMany می‌توانست health/shutdown موفق بدهد؛ در WAL ناقص، shutdown به assertion RocksDB می‌رسید.
5. integrity check، حتی با `verify_indexes=false`، شمارندهٔ هر collection را در همان snapshot با تعداد واقعی اسناد مقایسه می‌کند.
   fixture قدیمی migration نیز همراه سند legacy، شمارندهٔ صحیح می‌سازد؛ migration اجازهٔ عبور از store ناسازگار ندارد.
6. hookهای یک‌بارمصرف staging به update/delete گسترش یافتند؛ callback تست فقط در subprocess برای SIGKILL دقیق استفاده می‌شود و binding یا تنظیم سرویس ندارد.

## اجرا و شواهد

```bash
cmake --build build/debug --target nexora_doc_tests nexoradb_py --parallel 2
build/debug/tests/cpp/nexora_doc_tests \
  --gtest_output=json:build/cor007-native-REPEAT.json
python3 scripts/verify_failure_recovery.py \
  --probe build/debug/tests/cpp/nexora_recovery_crash_probe \
  --directory build/cor007-REPEAT
# اجرای خودکار همان campaign از CTest:
ctest --test-dir build/debug -R '^cpp.FailureRecoveryCrashMatrix$' --output-on-failure
```

هر اجرا زیرپوشهٔ تازهٔ `run-*` می‌سازد؛ report و stdout/stderr تمام childها حفظ می‌شوند.
خروجی شامل hash خود runner و probe، نام مرز، وضعیت ACK، policy و نتیجهٔ recovery است؛ exception/timeout یا نرسیدن به fault point اجرای ناموفق می‌دهد.
worker با `exec` جدا اجرا می‌شود؛ fork روی موتور زندهٔ چندنخی انجام نمی‌شود. فقط worker متعلق به همان اجرا کشته می‌شود.
تست‌ها در CTest و native quality قرار دارند؛ CI گزارش و logها را artifact می‌کند. campaign فرایندی در TSAN اجرا نمی‌شود.

شواهد محلی ۲۰۲۶-۱۰-۳۰، build نوع Debug:

- ۱۳۲ تست native موفق، بدون skip؛ شامل ۵۹ تست جدید failure/recovery.
- ۸۲ case موفق campaign: ۷۰ مرز crash، ۹ WAL ناقص و ۳ خرابی فیزیکی.
- ۴۱ تست Python مربوط به isolation، مدل مرجع، type semantics و migration روی binary جدید موفق.
- ۱۲ SIGKILL/reopen ورودی‌های native/default/durable/buffered، ASGI service و queued writer موفق.
- گزارش‌های نهایی: `build/cor007-native-acceptance-20261030.json`، `build/cor007-python-acceptance-20261030.xml`،
  `build/cor007-entrypoint-acceptance-20261030/report.json` و
  `build/debug/tests/cpp/cor007-recovery/run-3zi_7nk7/report.json`؛ مورد آخر از CTest در ۵۷٫۴۶ ثانیه پاس شد.
  اجرای پیش از اصلاح counter-underflow نیز در `build/cor007-crash-final-20261030/run-mgx4nyv5/report.json` حفظ شده است.
- اجرای ناموفق اولیه حفظ شده است: `build/cor007-native-20261030.json` و
  `build/cor007-crash-20261030/run-_i7z6_oy/report.json`؛ شش failure بومی latch و assertion در WAL ناقص از همان اجرا به regression تبدیل شدند.

## حدود پذیرش

این شواهد process crash و خطای قطعی روی storeهای کوچکِ متعلق به تست هستند؛ قطع برق، خرابی فیزیکی دستگاه،
تمام instructionهای RocksDB، همهٔ حالت‌های compaction/manifest، آزمون طولانی sanitizer و مقیاس ۱۰۰M بررسی نشده‌اند.
خرابی فیزیکی WAL/SST در این campaign برای Durable آزموده شده است؛ از crash سالم Buffered، تحمل corruption آن نتیجه نمی‌شود.
entry گم‌شده یا counter عددی اشتباه الزاماً در query سریع قابل تشخیص نیست؛ integrity scan صریح برای کشف خرابی منطقی لازم است.
COUNT سریع جای اسکن تمام اسناد را نمی‌گیرد. پس از مشاهدهٔ خرابی، نتیجهٔ موفق جعلی یا repair/skip خودکار مجاز نیست.
ACK گم‌شده پس از sync ممکن است commit کامل داشته باشد؛ retry خودکار، exactly-once و receipt اتمیک از این تست‌ها نتیجه نمی‌شوند.
atomic document+graph، replay گراف و recovery مشترک موتورهای دیگر در تسک‌های GRAPH/CORE جدا باقی می‌مانند.
هیچ DB کاربر migrate، repair یا حذف نشده است.
