# قرارداد دوام نوشتن — COR-003

پذیرش ۲۰۲۶-۱۰-۲۹؛ دامنهٔ پروفایل‌های Buffered/Durable، موتور **document** است.
قرارداد سند، ایندکس و شمارنده را شامل می‌شود؛ replication یا commit اتمیک document+graph از آن نتیجه نمی‌شود.

## انتخاب صریح و تنظیم مؤثر

| ورودی | انتخاب | مشاهدهٔ effective setting |
| --- | --- | --- |
| C++ و pybind native | `RocksSettings.durability`؛ پیش‌فرض جدید **Durable** | `DurabilityReport()` / `durability_report()` |
| سرویس admin/API | `NEXORADB_ROCKS_DURABILITY=durable` یا `buffered`؛ پیش‌فرض durable | startup INFO logger، `SYSTEM INFO;` و `SHOW PERFORMANCE;` |
| CLI Textual و dashboard | کلاینت سرویس‌اند، موتور جدا باز نمی‌کنند؛ از پروفایل سرور پیروی می‌کنند | اجرای `SYSTEM INFO;` در query، یا وضعیت performance با مجوز لازم |
| API JSON و BSON | هر دو همان engine و مسیر execute را مصرف می‌کنند | همان گزارش سرور؛ encoding تضمین را عوض نمی‌کند |
| lab benchmark | Durable صریح و اجباری؛ effective flags هنگام connect بررسی می‌شوند | `metadata.effective_durability` در کنار metadata قبلی |

پروفایل در طول عمر موتور immutable است؛ query برای تغییر runtime آن اضافه نشده است.
مقدار enum نامعتبر C++ و رشتهٔ نامعتبر سرویس خطا می‌دهند، نه fallback خاموش به Buffered.
متغیر محیط case-insensitive است؛ API تنظیمات Python رشتهٔ دقیق `durable`/`buffered` می‌پذیرد.
سیاست hardware هیچ‌یک از این flags را تغییر نمی‌دهد. binary فاقد API گزارش در سرویس/lab پیش از بازکردن storage رد می‌شود.

گزارش native از تنظیمات مؤثر ساخته می‌شود: `profile`، `wal_enabled`، `sync_on_ack`،
`manual_wal_flush`، مرز ACK، scope و فرض storage. `operation_metrics().write_path` نیز flags و شمار WAL sync را دارد.
startup باید پروفایل درخواستی و effective را برابر ببیند؛ عدم تطابق اجازهٔ startup موفق نمی‌دهد.

## مرز ACK

- **Durable:** WAL روشن، `sync=true` و `manual_wal_flush=false`؛ نتیجهٔ موفقِ write مستقل یا commit،
  پس از موفقیت write/commit همگام RocksDB برمی‌گردد. سند، ایندکس و شمارنده در همان batch/transaction تغییر می‌کنند.
- **Buffered:** WAL روشن، `sync=false` و `manual_wal_flush=false`؛ ACK پس از انتقال WAL به سیستم‌عامل است،
  نه تضمین انتقال به رسانهٔ پایدار. crash فقط فرایند با خرابی/خاموشی ماشین یکسان نیست؛ از موفقیت تست اول، تضمین دوم حاصل نمی‌شود.
- مقدار `bytes_per_sync` / `wal_bytes_per_sync` راهنمای range-sync است، نه جایگزین `sync=true` یا مرز ACK.
- موفقیت `insert_one_tx` و عملیات مشابه داخل transaction فقط **staging** است. دوام پس از `commit_transaction.success=true`
  معنا دارد؛ rollback و تراکنش commit‌نشده ACK نوشتن نهایی نیستند.
- API/CLI باید نتیجهٔ statement/commit را بخوانند. HTTP 200 به‌تنهایی ACK موفق همهٔ statementها نیست؛
  `statementErrors` و `raw.statements[].success` نیز مهم‌اند. driver خطای statement را exception می‌کند.
  درخواست چند statement بدون transaction لزوماً اتمیک نیست و statementهای قبلی ممکن است commit شده باشند.
- قطع transport پس از ارسال write، یا خطای I/O/sync/commit، **outcome unknown** است؛ ممکن است WAL بخشی از مسیر را طی کرده باشد.
  شکست به معنی «قطعاً هیچ اثری نداشته» نیست؛ قبل از retry وضعیت را با شناسهٔ پایدار reconcile کنید.
  validation failure پیش از commit با این وضعیت یکسان نیست. صحت گزارش خطا/health را نمی‌توان با retry پنهان جایگزین کرد.
- خطای sync در batch معمولی، exclusive BSON و commit صریح ACK موفق نمی‌دهد؛ خطای commit/rollback صریح نیز
  اکنون health I/O/corruption را latch می‌کند. shutdown پس از storage failure آن را با sync موفق ظاهری نمی‌پوشاند.
- `FlushStorage` عملیات maintenance است؛ بعداً flush‌کردن، ACK قبلی Buffered را به ACK از ابتدا Durable تبدیل نمی‌کند.
  مسیر idempotency موجود پیش از receipt مستقل، barrier محافظه‌کارانهٔ document و graph را نگه داشته است؛
  receipt مستقل جای transaction اتمیک میان SQLite/document/graph نیست.

این رفتار با توضیح رسمی [نوشتن sync و non-sync در RocksDB](https://github.com/facebook/rocksdb/wiki/Basic-Operations)
و [قرارداد WAL](https://github.com/facebook/rocksdb/wiki/Write-Ahead-Log-%28WAL%29) همخوان است.
تضمین Durable مشروط به درست‌عمل‌کردن filesystem، kernel/VM و دستگاه در پاسخ به sync است؛
این قرارداد به‌معنی مقاومت اثبات‌شده در برابر کنترلر معیوب، بیت‌روت یا storage دروغ‌گو نیست.

## graph و مرز تضمین

Graph WAL پروفایل مستقل `WalOptions` دارد و پیش‌فرض native/service آن `WalDurability::Sync` است؛
Buffered کردن document، graph را خودکار Async نمی‌کند. انتخاب صریح Sync/GroupCommit/Async در API graph
قابل انجام است و باید مستقل ثبت شود. `document_durability` عمداً گزارشی دربارهٔ WAL graph نیست.
پاس‌شدن crash document به معنی crash consistency مشترک موتور document و live graph نیست؛
cross-engine recovery و crash/rollback گراف در تسک‌های graph/COR بعدی و تست‌های اختصاصی آن باقی می‌مانند.

## تغییر سازگاری

پیش‌فرض قبلی native و سرویس Buffered بود؛ این تغییر آن را **Durable** می‌کند.
ممکن است latency/throughput نوشتنِ callerهایی که پروفایل را حذف می‌کردند تغییر کند؛ بهبود سرعت ادعا نمی‌شود.
اگر workload عمداً تضمین ضعیف‌تر را انتخاب می‌کند، Buffered را صریح تنظیم و در نتایج ثبت کنید:

```python
settings = native.RocksSettings()
settings.durability = native.DocumentDurability.Buffered  # انتخاب آگاهانه؛ نه حالت مقایسهٔ durable
engine = native.DocEngine(path, native.TransactionSettings(), settings)
print(engine.durability_report())
```

سرویس از `NEXORADB_ROCKS_DURABILITY=buffered` برای پردازش جدید استفاده می‌کند؛
پردازش درحال‌اجرا را در این تغییر restart نکردیم. benchmark موجود قبلاً Durable صریح بود؛
نتایج تاریخی به build تازه منتسب نشده‌اند. شمارهٔ نسخه در این تسک تغییر نکرده است.

## تست crash واقعی و شواهد

ابزار `scripts/verify_durability_crash.py` فقط child اختصاصی خودش را پس از دریافت ACK می‌کشد:

- پنج مسیر native-default، native-Durable، native-Buffered و service-Durable/Buffered، هرکدام دو تکرار؛ **۱۰ SIGKILL واقعی**.
- worker پیش از kill، flush/close/PrepareShutdown نمی‌کند. نتیجهٔ ACK در parent دریافت و سپس همان PID کشته می‌شود؛
  exit code باید `-SIGKILL` باشد. process دیگری یا سیستم میزبان restart/kill نمی‌شود.
- JSON single/batch، packed BSON، exclusive BSON روی index معمولی (بدون fallback ناشی از unique index)،
  update کلید ایندکس، delete، commit، rollback و staging commit‌نشده بررسی شدند.
- مسیر service از factory واقعی و endpointهای JSON/BSON با token واقعی در **ASGI TestClient، نه TCP** استفاده می‌کند؛
  `SYSTEM INFO` نیز از همان endpoint خوانده می‌شود. این آزمون kill موتور/فرایند API است، نه آزمون خرابی شبکه یا اجرای TUI.
- پس از reopen تمام اسناد و فیلدها، شمارنده، lookup ایندکسی و plan واقعی `index_equality` بررسی شدند؛
  سند/کلید ایندکس حذف‌شده و تراکنش rollback/commit‌نشده غایب بودند. هیچ missing/partial write تأییدشده دیده نشد.
- تست C++ خطای واقعی لایهٔ Env در WAL Sync/Fsync را تزریق می‌کند و شکست بدون ACK موفق را در سه مسیر می‌سنجد؛
  outcome پس از خطای sync را به‌اشتباه «قطعاً absent» اعلام نمی‌کند.

شواهد نهایی: `build/cor003-crash-final-20261028/report.json` و stdout/stderr هر child.
گزارش اولیهٔ پیش از اصلاح latch نیز در `build/cor003-crash-20261028/` حفظ شده و بازنویسی نشده است.
native SHA نهایی و ثبت‌شده در گزارش:
`7229422e01aafa7c3e474bf759e9bca6dd55360100d2fb40a06436f16caef1aa`.
commit پایه `3930b5ada1b7b3a715acaeacbe3c62b11af0c8a3` با working tree تغییرکرده است؛ commit تازه ساخته نشد.

تکرار کوتاه در PowerShell، با **پوشهٔ تازه**:

```powershell
wsl -d Ubuntu -- /home/parham/NexoraDB/build/release-0.4.1-install-test/bin/python /home/parham/NexoraDB/scripts/verify_durability_crash.py --directory /home/parham/NexoraDB/build/cor003-crash-REPEAT --repetitions 2
```

نتیجهٔ رگرسیون: **۶۶ تست native، ۶۳ تست Python مرتبط، ۶ pagination، ۵ API capacity و ۱ threading موفق**؛
research با opt-inهای native/TCP/cgroup **۲۸ موفق**؛ lab **۵۳ موفق و ۷ skip اختیاری**.
دو گروه تست native-only و package/API در processهای جدا اجرا شدند تا extension هم‌نام، package را shadow نکند؛
binary package و ریشه پس از rebuild همگام شدند و محافظ mismatch loader حذف نشد.
هشدار موجود Starlette/httpx ثبت شد؛ checker نسخه همچنان ۰٫۴٫۱ همخوان است.

**Power-loss آزموده نشده است**: قطع برق/VM-reset، filesystem fault campaign یا crash تصادفی حین تمام instructionها
در این شواهد نیست. تست چندساعته یا ۱۰M/۱۰۰M نیز اجرا نشد. این محدودیت به‌صورت `power_loss_tested=false`
در گزارش آمده است؛ نه اینکه kill فرایند را معادل آزمایش قطع برق اعلام کنیم.
