# قرارداد overload، fairness و shutdown — MEM-005/006

تاریخ: ۲۰۲۶-۱۰-۳۱. این پذیرش برای مسیر سرویس Python/ASGI و scheduler موجود گراف است. قرارداد سراسری workerهای همهٔ bindingها، embeddingها و pluginها در CON-005 همچنان باز است. policyها per-controller/per-service هستند؛ چند process یا چند ASGI app مستقل سقف مشترک ایجاد نمی‌کنند.

## admission و سهم منابع

`ExecutionController` در `nexoradb_admin/execution_control.py` صف، تعداد اجرای هم‌زمان، هزینه و مالک authenticated را محدود می‌کند. هزینه واحد policy است؛ اندازهٔ واقعی CPU، بایت یا RSS نیست و جای `ResourceBudget` حافظه را نمی‌گیرد. route زبانِ admin و API، از جمله BSON، controller همان app را استفاده می‌کنند. owner برای API از app_id تأییدشده و برای admin از subject توکن به دست می‌آید؛ کاربر هزینه یا maintenance lane را انتخاب نمی‌کند.

| حد پیش‌فرض | قرارداد |
|---|---|
| HTTP | ۱۶ درخواست تا پایان ارسال پاسخ/stream؛ body حداکثر ۲MiB، خواندن body حداکثر ۱۰ ثانیه؛ اضافه‌بار 503 |
| query | ۸ اجرای هم‌زمان؛ یک slot و ۸ واحد هزینه مخصوص کار کوچک، هفت slot و ۷۲ واحد مخصوص کار بزرگ |
| maintenance | یک اجرای مستقل و ۲۰ واحد هزینه؛ query سهم آن را مصرف نمی‌کند |
| app | دو اجرای query و حداکثر ۴۰ واحد؛ کار بزرگ حداکثر یک slot و ۳۲ واحد را مصرف می‌کند تا یک کار کوچک همان app امکان اجرا داشته باشد |
| صف | ۳۲ ورودی: حداکثر ۳۰ بزرگ، یک کوچک و یک maintenance؛ حداکثر هشت انتظار برای هر owner |
| انتظار | حداکثر ۲۵۰ms پیش‌فرض؛ deadline درخواست نیز رعایت می‌شود؛ پس از آن خطای overload صریح |

صف هر دسته مستقل است؛ پرشدن صف/هزینه/slot کار بزرگ، جای query کوچک یا maintenance را نمی‌گیرد. درون دسته، کار قدیمیِ موقتاً غیرقابل اجرا حداکثر هشت بار یا تا سن ۱۰۰ms قابل سبقت است؛ سپس دسته برای آن drain می‌شود. شمارش سبقت و FIFO به ترتیب ورود زیر lock وابسته‌اند. کوچک‌ها هم نمی‌توانند سهم رزروشدهٔ کار بزرگ را اشغال کنند. این تفکیک عامدانه ممکن است هنگام خالی بودن یک دسته، ظرفیت بلااستفاده بگذارد.

هزینهٔ server از اندازهٔ UTF-8 query، با پایهٔ `1 + bytes // 16384` و AST برآورد می‌شود. SELECT دقیق با شرط `_id = string` و بدون join و COUNT بدون شرط، در اندازهٔ معمول کوچک‌اند؛ SELECT عمومی، COUNT فیلترشده، build، job، index و mutationهای update/delete حداقل ۱۶ واحد دارند. هزینهٔ بزرگ حداکثر ۳۲ است. این تخمین محافظه‌کارانه است، نه cost optimizer. parse با سقف اندازهٔ ورودی و admission سراسری HTTP انجام می‌شود و کل block قبل از دسترسی engine از نظر مجوز بررسی می‌شود. quota واقعی حافظه، page و response همچنان مستقل enforce می‌شود. نتیجهٔ overload برابر HTTP 503 با `Retry-After: 1`، کد `overloaded` و outcome `not_started` است؛ صف نامحدود یا retry مخفی نداریم.

monitoring سرویس از maintenance lane و limiter مستقل یک‌تایی استفاده می‌کند و در drain ثبت می‌شود. worker migration سند نیز زیر بار پیوسته، پس از حداکثر پنج ثانیه فرصت اجرای chunk تا ۱۰۰ سند/۲۵۶KiB می‌گیرد؛ در حالت idle سقف قبلی ۱۰۰۰ سند/۴MiB باقی است. `migration_foreground_grants_total` تعداد این فرصت‌ها را گزارش می‌کند. سقف بایت chunk، رفتار موجود دربارهٔ یک سند بزرگ را تغییر نمی‌دهد. این زمان، فاصلهٔ فرصت زمان‌بندی است؛ زمان انتظار lock یا I/O به آن افزوده می‌شود. اجرای maintenance دلخواه در embedding باید صریحاً `controller.acquire(owner, cost, lane="maintenance")` بگیرد.

native `JobScheduler` تعداد worker، صف، رزرو بایت و تعداد submitterهای منتظر را محدود می‌کند. اولویت قبلی برای burst حفظ شده ولی قدیمی‌ترین job پس از حداکثر هشت dispatchِ سبقت‌گیر یا رسیدن سن صف به ۱۰۰۰ms ترجیح داده می‌شود. هر دو حد با `SchedulerOptions` قابل تنظیم‌اند. این تضمین برحسب dispatch است؛ job در حال اجرای غیرهمکار، lock متوقف یا I/O معلق با تغییر اولویت preempt نمی‌شود. jobهای همکار از cancellation/deadline موجود snapshot استفاده می‌کنند.

## تنظیم و telemetry

تنظیم‌های سرویس: `NEXORADB_EXECUTION_QUERY_SLOTS=8`، `NEXORADB_EXECUTION_PER_APP=2`، `NEXORADB_EXECUTION_QUEUE_SIZE=32`، `NEXORADB_EXECUTION_WAIT_MS=250`، `NEXORADB_EXECUTION_TIMEOUT_MS=30000` و `NEXORADB_SHUTDOWN_DRAIN_MS=30000`. برای حفظ سهم کوچک، دو مورد اول حداقل ۲ و صف حداقل ۳ است؛ startup مقدار خارج از محدوده را رد می‌کند. ساخت مستقیم `AdmissionPolicy(small_slots=0)` برای آزمون/embedding ممکن است و تضمین سهم کوچک را حذف می‌کند؛ سرویس این تنظیم را ارائه نمی‌کند.

`GET /system/execution` با access token معتبر admin UI، stage و eventهای shutdown و گزارش admission را می‌دهد؛ همان گزارش در `app.state.lifecycle.report()` در دسترس embedding است. گزارش owner/token/query خام ذخیره نمی‌کند. فیلدهای کلیدی:

- `queued`, `running`, `running_cost`, `maintenance_running`, `oldest_wait_seconds`, `stopping`؛
- `admitted_total`, `maintenance_admitted_total`, `rejected_total`, `expired_total`, `waited_total`؛
- `cancelled_total` برای cancellation در انتظار admission؛ `wait_seconds_sum/max` شامل زمان acquisitionهای فوری نیز هستند؛
- native scheduler: `admission_waiters`, `admitted_total`, `rejected_total`, `waited_total`, `cancelled_total`, `timed_out_total`, `fairness_grants_total`, `admission_wait_ms_sum`, `queue_ms_sum/max`, `oldest_queue_ms`؛ از binding و `GraphManager.memory_report()` قابل دریافت‌اند.

## لغو و outcome نوشتن

`Cancellation` deadline و Event دارد. scope درخواست با ContextVar به worker منتقل می‌شود. `DocEngine.execution_scope(timeout_ms)` کنترل native را در thread فعلی نصب می‌کند؛ scope باید روی همان thread و با nesting صحیح بسته شود. `cancel()` از thread دیگر مجاز است. stream کنترل را نگه می‌دارد و برای هر batch، حتی روی worker متفاوت، نصب می‌کند. کنترل به options snapshot/job نیز منتقل می‌شود.

safe pointها شامل بین statementها، هر سند در scan/match، پیمایش index، staging mutation/transaction، پیش از commit، و نقاط موجود ساخت/پیمایش snapshot گراف هستند. sort/spill از callback لغو و cleanup قرارداد MEM-004 استفاده می‌کند. لغو اجباری داخل RocksDB commit، syscall، lock و کد plugin انجام نمی‌شود. لغو فوریِ disconnect برای همهٔ transportها تضمین نشده است: disconnect مشاهده‌شده و cancellation task ثبت می‌شوند؛ deadline و signal نیز کنترل را فعال می‌کنند.

| زمان لغو/خطا | outcome |
|---|---|
| پیش از admission | اجرای جدید شروع نمی‌شود؛ خطای overload یا cancellation |
| scan یا پاسخ صفحه‌ای | exception/خطای صریح؛ نتیجهٔ buffered ناقص موفق نیست؛ session حتی در exception داخل matcher بازنشسته می‌شود |
| پیش از commit تراکنش | rollback در teardown؛ rollback پس از cancel نیز در دسترس است |
| حین commit RocksDB | تا نتیجهٔ واقعی commit صبر می‌کنیم؛ پس از commit موفق، check لغو اضافه نمی‌شود |
| بین statementهای مستقل | prefix تکمیل‌شده حفظ می‌شود؛ لغو statement بعدی، write قبلی را rollback نمی‌کند |
| خطای I/O commit یا از دست رفتن ACK | cancellation ادعای rollback ایجاد نمی‌کند؛ outcome ممکن است نامعلوم باشد و قرارداد recovery/idempotency قبلی اعمال می‌شود |
| graph build یا bulk chunked | side effect یا prefix ممکن است باقی بماند؛ به معنای atomic transaction چند statement یا rollback عمومی گراف نیست |

HTTP لغو query، کد `execution_cancelled` و `completedStatements` با نوع، success و outcome هر statement برمی‌گرداند. writeهای staged پس از commit به `committed` و پس از rollback به `rolled_back` تبدیل می‌شوند. transport قطع‌شده ممکن است این پاسخ را دریافت نکند؛ caller نباید صرفاً از قطع اتصال، عدم commit را نتیجه بگیرد. streamی که prefix داده دریافت کرده، تنها با marker پایان و بدون خطا کامل است.

## ترتیب shutdown و signal

`ServiceLifecycle` روی lifespan هر دو app نصب است. `nexoradb-server` در حالت عادی از `ControlledServer` استفاده می‌کند: SIGTERM/SIGINT پیش از drain خود Uvicorn، admission را می‌بندد و tokenهای فعال را cancel می‌کند. signal تکراری در این entrypoint worker را رها نمی‌کند. اجرای مستقیم `uvicorn ...` یا حالت توسعهٔ `--reload` همچنان lifespan را دارد، اما hook زودهنگام signal این entrypoint را ندارد؛ برای آن‌ها embedding باید `stop_admission()` را قبل از drain فراخوانی کند.

ترتیب ثابت:

1. بستن admission و بیدارکردن waiterها؛ توقف admission حلقهٔ monitoring و درخواست cancellation؛
2. drain همهٔ requestها تا پایان واقعی response و همهٔ permitهای اجرای query/maintenance؛
3. توقف و join حلقهٔ monitoring و workerهای scheduler؛ job/captureها پیش از آزادشدن reservation از بین می‌روند؛
4. آزادکردن snapshot cache گراف، checkpoint WAL و durable sync گراف؛
5. توقف migration/page reaper، آزادکردن sessionهای سند، flush storage و SyncWAL؛
6. بستن storage/WAL گراف و رهاکردن referenceهای متعلق به سرویس برای close سند.

teardown executor تراکنش باز را rollback، snapshotهای محلی را رها و jobهای باقی‌مانده را cancel/join می‌کند. middleware پس از cancellation task، اجرای downstream را تا پایان واقعی دنبال می‌کند؛ slot یک worker هنوز فعال زودتر آزاد نمی‌شود. reference خارجی به engine/snapshot می‌تواند lifetime را بیشتر کند. embedding/native مستقیم موظف است پیش از `prepare_shutdown` همهٔ callerهای خودش را quiesce و drain کند؛ service نمی‌تواند caller خارج از مالکیتش را بشمارد.

deadlineِ drain یک kill timer نیست. اگر تمام نشود، stage برابر `failed` با `resources_retained=true` ثبت و خطا منتشر می‌شود؛ flush/close جلو نمی‌رود. پس از شروع barrier native، آن barrier abandon نمی‌شود؛ I/O متوقف می‌تواند پایان shutdown را عقب بیندازد. graph sync یا document flush ناموفق نیز `closed` گزارش نمی‌شود؛ خطای health قبلی باعث retry ضمنی WAL نمی‌شود. SIGKILL اجرای finally را تضمین نمی‌کند و به recovery پس از crash متکی است.

eventهای مرحله، زمان monotonic، علت signal، درخواست cancellation، failure و نگهداری منابع در یک ring حداکثر ۳۲تایی و logger `service_shutdown` ثبت می‌شوند. این ابزار مرحلهٔ توقف را نشان می‌دهد؛ referenceهای embedding و انتظار بی‌پایان plugin را به‌عنوان «بسته شد» پنهان نمی‌کند.

## شواهد و بازتولید

آزمون‌های policy، پرشدن صف/هزینهٔ بزرگ در کنار اجرای کوچک و maintenance، FIFO/سبقت محدود، deadline، shutdown waiterها، عدم release زودهنگام worker، شکست graph/document barrier و drain timeout وجود دارند. integration native rollback قبل از commit، حفظ write قبلی، scope تودرتو، لغو stream روی thread دیگر و deadline scan بی‌تطبیق را می‌سنجد. یک نشت snapshot در exception داخل matcher توسط همین آزمون پیدا و با guardِ تمام مسیرهای خروج ناموفق رفع شد.

۱۳۳ آزمون document/recovery و ۸۴ آزمون graph موفق شدند؛ مجموعهٔ Python مرتبط و regression شامل ۱۱۴ آزمون موفق و یک skip مربوط به binary آرشیوی reader قدیمی است. آزمون deadline scan چرخهٔ COUNT/page را پنج بار با بررسی release پس از هر چرخه تکرار می‌کند. artifactهای محلی در `build/mem005006-{python,native-doc,native-graph}-final-20261031.*` هستند. CI تست‌های متمرکز و probe signal را اجرا و artifactها را نگه می‌دارد.

`scripts/verify_execution_lifecycle.py` یک process و DB اختصاصی می‌سازد، cursor واقعی و write staged را باز نگه می‌دارد، SIGTERM می‌فرستد و خروج موفق، rollback، release، flush و reopen را بررسی می‌کند. نتیجه در `build/mem005006-signal-verified-20261031/report.json` با hash binary/source ثبت شده است. هیچ store کاربر هدف probe نیست. escalation احتمالی فقط برای child متعلق به probe و با فایل جدا ثبت می‌شود؛ در اجرای موفق escalation رخ نداد.

```sh
python3 -m pytest -q tests/test_execution_lifecycle.py tests/test_api_capacity_streaming.py tests/test_query_contracts.py
python3 scripts/verify_execution_lifecycle.py --directory build/mem005006-new-run \
  --native build/debug/nexoradb.cpython-310-x86_64-linux-gnu.so
```

این شواهد ادعای hard RSS، fairness توزیع‌شده، preemption کد دلخواه یا پذیرش shutdown روی همهٔ سیستم‌عامل‌ها نیست؛ بررسی عمومی executor و سقف workerهای تمام مسیرها در CON-005 باقی است.
