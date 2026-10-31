# رودمپ پژوهشی و متن‌باز NexoraDB

تاریخ تدوین: ۲۰۲۶-۱۰-۲۲

هدف: ساخت دیتابیس document+graph با صحت، امنیت و عملکرد قابل اندازه‌گیری، سپس ارائهٔ نسخهٔ بازتولیدپذیر در نمایشگاه پروژه‌های متن‌باز. تمرکز فعلی فروش، SLA تجاری یا الزامات قراردادی نیست. حجم کد معیار پیشرفت نیست؛ نتیجهٔ صحیح، منابع مصرفی، تجربهٔ استفاده و شواهد مستقل معیارند.

این سند برنامهٔ کار است. هیچ چک‌باکسی صرفاً به دلیل وجود کد یا توضیح قبلی تکمیل نشده است. وضعیت فعلی هر تسک باید هنگام شروع با همان commit و working tree بررسی شود. بخشی از زیرساخت‌ها قبلاً پیاده شده‌اند؛ پیش از تغییر، آن‌ها را شناسایی و تکمیل کنید.

مراجع داخلی:

- [بک‌لاگ صنعتی و شواهد کد](INDUSTRIAL_COMPETITIVENESS_BACKLOG.md)
- [فاز صفر اندازه‌گیری](../nexora-scale-lab/docs/PHASE_ZERO.md)
- [فاز یک حافظه](../nexora-scale-lab/docs/PHASE_ONE_MEMORY.md)
- [تحقیق بارگذاری](cpp/TARGET_150K_INGESTION.md)

## قواعد اجرای تسک توسط AI

1. ابتدا دستورهای مخزن، وضعیت تغییرات موجود، کد مرتبط و تست‌ها را بخوان. تغییرات کاربر را حفظ کن.
2. یک شناسه یا زیرتسک مشخص را اجرا کن. والدهای بزرگ را در چند تغییر قابل بازبینی تکمیل کن.
3. برای bug، تست بازتولید یا شاهد قابل تکرار بساز؛ برای بهینه‌سازی، baseline ثبت کن.
4. قرارداد صحت، دوام نوشتن، نوع داده، ownership، GIL و منابع را پیش از تغییر مشخص کن.
5. بهبود throughput با حذف validation، خاموش‌کردن WAL یا تغییر پنهان تضمین‌ها قابل قبول نیست.
6. تغییر format، API یا رفتار نیازمند مستند سازگاری و در صورت لزوم migration است.
7. تست متناسب با تغییر اجرا کن. برای تغییر عملکرد، قبل/بعد هم‌شرایط و اثر روی p99 و منابع گزارش کن.
8. تست‌های بزرگ ۱۰M/۱۰۰M و تست‌های طولانی را تنها با دستور صریح کاربر اجرا کن؛ ابتدا dry-run و دستور اجرا آماده شود.
9. داده، نتیجهٔ تاریخی و manifest قدیمی را بازنویسی یا به نسخهٔ جدید منتسب نکن. دادهٔ باقی‌مانده را بدون درخواست حذف نکن.
10. اگر معیار پذیرش هنوز شاهد ندارد، تسک را باز بگذار و مانع را دقیق بنویس. وابستگی حل‌نشده را پنهان نکن.

### قالب درخواست قابل کپی برای AI

```text
تسک [شناسه] را از docs/OPEN_SOURCE_RESEARCH_ROADMAP_FA.md اجرا کن.
ابتدا وضعیت کد و وابستگی‌ها را بررسی کن؛ کار انجام‌شده را از صفر بازنویسی نکن.
شاهد مسئله و قرارداد رفتار صحیح را ثبت کن، سپس تغییر محدود و قابل بازبینی بده.
تست واقعی و معیار پذیرش همان تسک را بررسی کن.
در پایان فایل‌های تغییرکرده، نتیجهٔ تست، شواهد عملکرد، محدودیت و کار باقی‌مانده را گزارش کن.
فقط اگر معیار پذیرش محقق شده چک‌باکس را تیک بزن و مسیر شواهد را درج کن.
تست بزرگ را بدون دستور صریح اجرا نکن.
```

### ثبت شواهد برای هر تسک

زیر تسک تکمیل‌شده این قالب را اضافه کنید؛ گزارش می‌تواند در فایل جداگانه باشد:

```text
وضعیت: باز / در حال اجرا / مسدود / تکمیل / خارج از محدوده
commit پایه و تغییرات working tree:
قرارداد و علت تغییر:
مسیر فایل‌ها و تست‌ها:
فرمان و نتیجهٔ تست:
مسیر دادهٔ خام و گزارش قبل/بعد، اگر مرتبط است:
نسخهٔ binary، تنظیمات، سخت‌افزار و seed:
ریسک، محدودیت و پیگیری:
تاریخ بررسی:
```

خارج از محدوده بودن به معنی تکمیل نیست. تیک والد تنها پس از تکمیل زیرتسک‌های لازم و پذیرش end-to-end زده شود.

## اولویت و ترتیب وابستگی

- **P0:** صحت داده، امنیت قابل دسترس یا قرارداد پایه؛ قبل از ادعای مرتبط حل شود.
- **P1:** اجرای قابل اتکا و مزیت پژوهشی اصلی.
- **P2:** گسترش قابلیت و تجربهٔ استفاده بر اساس کاربرد منتخب.
- **P3:** تحقیق چندنودی و توسعهٔ افقی؛ نیازمند طراحی مستقل.

مسیر نخست پیشنهادی:

`RES-001..005 → COR-001..003 → MEM-007 → READ-001/002 → CON-001/002`

در ادامه:

`COR-004..007 → WRITE-001/002 → GRAPH-001..005 → EVAL-001..004 → DEMO-001..003`

تسک‌های امنیت در طول تمام مراحل اجرا شوند. بهینه‌سازی هر مسیر به قرارداد صحت همان مسیر وابسته است. تسک‌های زیر پیش‌نیازهای اصلی را مشخص می‌کنند؛ AI باید وابستگی تازهٔ کشف‌شده را هم ثبت کند.

## مرحلهٔ ۰ — پرسش پژوهشی و مبنای اندازه‌گیری

وضعیت شروع و شواهد ۲۰۲۶-۱۰-۲۸: [پروندهٔ مرحلهٔ صفر](research/README.md).
تیک‌های زیر فقط خروجی‌های واقعاً آماده را نشان می‌دهند؛ تسک والد تا پذیرش کامل باز می‌ماند.

- [x] **RES-001 · P1 — انتخاب کاربردهای هدف**
  - [x] سه سناریوی document OLTP، query ترکیبی document+graph و اجرای کم‌حافظه را با داده و query مشخص تعریف کن.
  - [x] حجم داده، concurrency، نسبت read/write، محدودیت RAM و خروجی مورد انتظار را بنویس.
  - خروجی: مشخصات workloadهای هدف در سند versioned.
  - پذیرش: هر سناریو قابل اجرا و دارای مرجع صحت باشد؛ موارد خارج از محدوده روشن باشند.
  - شواهد: [قرارداد v1](research/PHASE0_WORKLOADS_V1_FA.md) و [پذیرش ۲۰۲۶-۱۰-۲۸](research/STATUS_PHASE0_FA.md#پیگیری-و-پذیرش-فنی-۲۰۲۶-۱۰-۲۸)؛ هر سه سناریو با ۱/۱۰ worker هم‌زمان و oracle کامل اجرا شدند؛ W3 در cgroup مستقل. این تیک به معنی پاس حجم‌های بزرگ یا فرضیهٔ عملکرد نیست.

- [x] **RES-002 · P1 — ماتریس مقایسهٔ رقبا**
  - وابسته به: RES-001.
  - [x] نسخه و edition محصولات MongoDB، PostgreSQL، SQL Server، Oracle و Neo4j را مشخص کن.
  - [x] مدل داده، durability، isolation، transport، ایندکس و منابع مقایسه را با مستند رسمی ثبت کن.
  - خروجی: ماتریس قابلیت و شرایط قابل مقایسه؛ قابلیت فاقد معادل با N/A مشخص شود.
  - پذیرش: اختلاف تضمین یا deployment پنهان نماند؛ نتیجهٔ یک subset به کل محصول تعمیم داده نشود.
  - شواهد: [ماتریس، نسخه‌های دقیق هدف و منابع رسمی](research/COMPARISON_MATRIX_V1_FA.md). تکمیل **سند طرح** است، نه نصب یا اجرای پنج محصول؛ runtime/digest و adapterهای ناموجود صریح‌اند و پیش‌شرط مقایسهٔ آتی باقی می‌مانند.

- [x] **RES-003 · P1 — فرضیه‌های قابل رد**
  - وابسته به: RES-001/002.
  - [x] برای سرعت، latency، memory، امنیت و کاربرد، ادعاهای محدود و آزمون‌پذیر بنویس.
  - [x] معیار قبول/رد و baseline هر ادعا را قبل از آزمایش تعیین کن.
  - خروجی: جدول فرضیه، متغیر مستقل، متغیرهای کنترل و معیار نتیجه.
  - پذیرش: «برتر در همه‌چیز» ادعای آزمایش نباشد؛ نتیجهٔ منفی هم قابل ثبت باشد.
  - شواهد: [H1..H6 و قواعد قبول/رد/نامعین](research/HYPOTHESES_V1_FA.md)، ثبت immutable در `build/research-preregistered-20261028/registration.json`. baseline artifact ثابت است؛ دادهٔ کنترل فعلی noisy و برای inference نامجاز است. **هیچ فرضیه‌ای پذیرفته نشده است.**

- [ ] **RES-004 · P1 — آزمایش A/B بازتولیدپذیر**
  - وابسته به: RES-003.
  - [x] ابزار موجود را برای ثبت سورس، binary، config، host، seed و ترتیب اجرا بررسی و تکمیل کن.
  - [x] تکرار مستقل، baseline ثابت، A/B/A در صورت drift و گزارش نوسان فراهم کن.
  - خروجی: دستورهای plan/run/analyze و نمونهٔ گزارش معتبر.
  - پذیرش: تغییر عملکرد از نوسان محیط تفکیک شود؛ تاریخچه بازنویسی نشود.
  - وضعیت: ابزار، snapshot سورس/binary، انتخاب و تطبیق native واقعی، CI95 و history حفظ‌شده آماده‌اند؛ **پذیرش والد باز است** چون کنترل A/A آخر هنوز حدود ۲۳٫۶٪ drift داشت و کالیبراسیون ۲۰۰۰عملیاتی هم نامعین بود. threshold برای سبزکردن نتیجه عوض نشد؛ [شواهد و ادامه](research/STATUS_PHASE0_FA.md#پیگیری-و-پذیرش-فنی-۲۰۲۶-۱۰-۲۸).
  - ادامهٔ آماده: [پروفایل پذیرش والدها و دستور اجرا](research/PARENT_ACCEPTANCE_FA.md)؛ warm-up، affinity و telemetry اضافه شدند و wiring کوتاه اجرا شد. آزمون طولانی عمداً اجرا نشده؛ تیک والد منوط به gates واقعی `acceptance.json` است.

- [ ] **RES-005 · P1 — تفکیک هزینهٔ لایه‌ها**
  - وابسته به: RES-004.
  - [x] آزمایش مستقیم C++، binding پایتون و API شبکه را برای عملیات مشترک بساز.
  - [x] منابع مولد و موتور را با scope صحیح گزارش کن؛ native هم‌پردازش را دوباره‌شماری نکن.
  - خروجی: breakdown هزینه و محدودیت attribution.
  - پذیرش: هزینهٔ TCP، serialization و generator با هزینهٔ موتور اشتباه نشود.
  - وضعیت: [سه لایهٔ واقعی با fixture و oracle یکسان](research/LAYER_ATTRIBUTION_V1_FA.md) پاس شده‌اند؛ گزارش sampled PSS/CPU و scopeها آماده است. **والد تا رفع وابستگی RES-004 و گزارش تکرارپذیر عددی باز می‌ماند**؛ تفاضل p99 به‌عنوان هزینهٔ خالص TCP معرفی نشده است.
  - ادامهٔ آماده: همان پروفایل پذیرش، پنج تکرار مستقل سه‌لایه و کنترل CV را اجرا می‌کند؛ تست کوتاه و unit test به‌جای شاهد تکرارپذیری پذیرفته نشده‌اند.

## مرحلهٔ ۱ — قرارداد و صحت پایه

- [x] **COR-001 · P0 — یکسان‌سازی pagination و stream**
  - محل شروع: `core/DocEngine.cpp`، `bindings/nexoradb_bind.cpp` و adapterهای lab.
  - [x] ناسازگاری سقف ۱۰۰۰ هسته و پذیرش ۴۰۹۶ در stream را در همهٔ ورودی‌ها رفع کن.
  - [x] سقف تعداد و بایت و رفتار سند بزرگ را در قرارداد مشترک ثبت کن.
  - خروجی: API و مستند همخوان؛ خطای زودهنگام برای درخواست نامعتبر.
  - پذیرش: مرزهای ۱۰۰۰/۱۰۰۱/۴۰۹۶، صفحهٔ خالی، سند بزرگ و چندصفحه بدون رکورد تکراری/گم‌شده تست شوند.
  - شواهد ۲۰۲۶-۱۰-۲۸: [قرارداد، سازگاری و فرمان تست](cpp/PAGINATION_CONTRACT.md)؛ ۶۱ تست native، ۵ تست binding واقعی و ۳ تست مرزی lab پاس شدند. تعداد ۱..۱۰۰۰ و بایت ۲..۶۴ MiB در ورودی رد/قبول می‌شوند؛ سند بزرگ خطا می‌دهد نه افتادگی خاموش. COR-002 و مقیاس بزرگ همچنان بازند.

- [x] **COR-002 · P0 — snapshot برای اسکن طولانی**
  - وابسته به: COR-001.
  - [x] idle timeout، عمر مطلق و حق ادامهٔ اسکن فعال را تعریف کن؛ سقف پنج‌دقیقه‌ای فعلی را بررسی کن.
  - [x] TTL را با ساعت قابل تزریق تست کن؛ snapshot رهاشده و snapshot در حال مصرف را تفکیک کن.
  - خروجی: قرارداد lease/close/expiry برای native، export و validation.
  - پذیرش: اسکن طولانی صحیح باشد؛ expiration نتیجهٔ ناقص را موفق گزارش نکند؛ pinned memory محدود و قابل مشاهده بماند.
  - شواهد ۲۰۲۶-۱۰-۲۸: [قرارداد lease/close/expiry و تست‌ها](cpp/SNAPSHOT_LEASE_CONTRACT.md)؛ ۶۴ تست native (۹ pagination)، ۶ binding، ۲۵ API/ACL/memory/threading و ۲۸ research پاس شدند؛ lab شامل ۵۳ موفق و ۷ skip اختیاری است. زمان با ساعت ثابت تزریقی پیش رفت، نه اجرای چندساعته. idle=۱۲۰s، native بدون cutoff مطلق و HTTP با deadline مستقل ۱۲۰s؛ ۱۲۸ snapshot زنده شامل retired/in-flight. سقف تعداد و صفحه bounded است؛ hard byte quota به‌ازای snapshot/RSS و retention دیسک ادعا نمی‌شود و محدودیت آن در قرارداد آمده است.

- [x] **COR-003 · P0 — دوام صریح در تمام ورودی‌ها**
  - محل شروع: `core/RocksTuning.h` و `NexoraDB/src/nexoradb_admin/native.py`.
  - [x] انتخاب و effective setting مربوط به Buffered/Durable را در سرویس، CLI، native و benchmark بررسی کن.
  - [x] ACK و خطای commit را مستند و در crash واقعی بررسی کن.
  - خروجی: تنظیمات معتبر و قرارداد دوام یکسان در لایه‌ها.
  - پذیرش: write تأییدشده مطابق تضمین اعلام‌شده پس از crash/reopen بازیابی شود.
  - شواهد ۲۰۲۶-۱۰-۲۹: [قرارداد دوام، ACK، migration و محدودیت‌ها](cpp/DOCUMENT_DURABILITY_CONTRACT_FA.md)؛ پیش‌فرض native/service به Durable تغییر کرد، Buffered صریح باقی ماند، گزارش مؤثر در native/SYSTEM INFO/SHOW PERFORMANCE و metadata lab اضافه شد. ۱۰ SIGKILL/reopen واقعی در پنج مسیر با oracle کامل سند/ایندکس/count پاس شد؛ `build/cor003-crash-final-20261028/report.json`. خطای sync در batch/exclusive/commit ACK موفق نمی‌دهد و latch خطای commit صریح اصلاح شد. ۶۶ native و ۶۳ Python مرتبط موفق؛ هیچ power-loss، replication یا atomic document+graph از این تیک ادعا نمی‌شود.

- [x] **COR-004 · P0 — semantics نوع و مقایسه**
  - [x] equality/order برای int/float، حدود عدد، null/missing، رشته، array و مقدار نامعتبر تعریف کن.
  - [x] evaluator، index codec و unique constraint را با همان corpus تطبیق بده.
  - خروجی: جدول semantics و تست differential.
  - پذیرش: scan=index؛ تغییر encoding دارای version و migration معتبر باشد.
  - پذیرش ۲۰۲۶-۱۰-۳۰: [قرارداد جاری و corpus](cpp/TYPE_COMPARISON_SEMANTICS_FA.md) و [مهاجرت کپی آفلاین v3](cpp/INDEX_V3_MIGRATION_FA.md). codec/unique برای int/double/±zero canonical شد؛ EQ عددی ایندکسی واقعی، golden و reader fence/manifest نسل ۳ اضافه شد. ۷۳ native موفق؛ سه XFAIL قبلی اکنون تست عادی موفق‌اند؛ insert/update race، delete/reuse، rollback batch و compound آزموده شدند. migration با binary واقعی قبلی، manifest نسل ۲ و بدون manifest، collision، SIGKILL، رد مقصد ناقص و حفظ source بررسی شد؛ ۱۲ crash/reopen دوام نیز موفق است. range عددی و prefix مرکب sparse ناامن همچنان full scan صحیح‌اند؛ این تیک ادعای range سریع، isolation عمومی، migration graph یا آزمون ۱۰۰M نیست. هیچ DB کاربر migrate نشده است.

- [x] **COR-005 · P0 — isolation و constraints هم‌زمان**
  - وابسته به: COR-003/004.
  - [x] سطح isolation واقعی و رفتار snapshot/read-your-writes را مستند کن.
  - [x] lost update، write skew، phantom، unique race و child-insert/parent-delete را با interleaving کنترل‌شده بررسی کن.
  - خروجی: history checker و قرارداد conflict/deadlock/retry.
  - پذیرش: historyها با سطح اعلام‌شده سازگار باشند؛ invariant داده نقض نشود.
  - پذیرش محدود ۲۰۲۶-۱۰-۳۰: [قرارداد isolation و محدودیت‌ها](cpp/ISOLATION_CONSTRAINTS_FA.md)، `scripts/transaction_history.py` و ۱۸ تست isolation/constraint. سطح واقعی Read Committed با RYW است، نه SI/Serializable؛ lost application update و write skew مجاز توسط oracle تشخیص داده می‌شوند، نه اینکه پنهان/رفع‌شده ادعا شوند. locking point read با `for_update=True` و کدهای conflict/deadlock/timeout اضافه شد. FK در store اولیهٔ معتبر با guard تراکنشی، revalidation و reverse RESTRICT محافظت می‌شود؛ دو ترتیب child/parent برای _id و non-id، reopen، BSON fallback، bulk rollback و DDL آزموده شدند. قفل FK فعلاً store-wide و reverse scan گران است؛ predicate transaction، invariant دلخواه چندسندی و audit orphan تاریخی از این تیک نتیجه نمی‌شوند. ۷۳ native، ۱۲۹ Python مرتبط و ۱۲ crash/reopen موفق؛ بدون تست سنگین یا تغییر DB کاربر.

- [x] **COR-006 · P0 — مدل مرجع مستقل عملیات**
  - وابسته به: COR-004/005.
  - [x] مدل سادهٔ مستقل برای insert/update/delete/query و تراکنش subset منتخب بساز.
  - [x] propertyهای bulk=single، scan=index، replay=original و codec roundtrip را تولید و بررسی کن.
  - خروجی: corpus با seed و کوچک‌سازی failure.
  - پذیرش: هر failure قابل بازتولید و به regression دائمی تبدیل شود.
  - پذیرش محدود ۲۰۲۶-۱۰-۳۰: [قرارداد مدل و اجرای seed/replay](cpp/REFERENCE_MODEL_FA.md)؛ مدل مستقل standard-library با Fraction، runner سه مسیر scan/index/transactional-single و corpus versioned ساخته شد. insert/set/delete/scalar EQ، bulk اتمیک، commit/rollback و codec roundtrip در subset مشخص بررسی شدند؛ bulk=single یعنی singleهای داخل یک transaction، نه چند commit مستقل. ۱۲ seed × ۸۰ operation سطح بالا، هر corpus دو بار روی store تازه اجرا و موفق شد. artifactهای corpus/report با hash مدل/runner/native حفظ می‌شوند؛ original/minimized و replay همان failure با fault تستی تأیید شد، fixture دائمی و loader بدون skip آماده است. ۱۲ تست مدل موفق؛ نبود failure واقعی در corpus محدود، اثبات نبود باگ عمومی نیست. graph، query عمومی، range و هم‌زمانی arbitrary خارج از subset هستند.

- [x] **COR-007 · P0 — خطای دیسک و crash**
  - وابسته به: COR-003/005/006.
  - [x] تست‌های موجود ENOSPC، EACCES، corruption و crash را به مسیرهای پوشش‌نداده گسترش بده.
  - [x] مرزهای document/index/counter/WAL/ACK را fault-inject کن.
  - خروجی: ماتریس failure و نتیجهٔ recovery.
  - پذیرش: partial commit، پاسخ موفق کاذب یا skip خاموش دادهٔ خراب رخ ندهد.
  - پذیرش محدود ۲۰۲۶-۱۰-۳۰: [ماتریس failure/recovery، اصلاحات و شواهد](cpp/FAILURE_RECOVERY_MATRIX_FA.md). تزریق WAL و staging در ۹ مسیر document گسترش یافت؛ ۱۳۲ native بدون skip، ۴۱ Python مرتبط و ۸۲ case فرایندی شامل ۷۰ مرز crash، ۹ WAL ناقص و ۳ خرابی فیزیکی موفق شدند. ۱۲ crash ورودی‌های native/service/queued نیز روی binary نهایی پاس شد. skip ارجاع dangling ایندکس، COUNT صفر کاذب، counter منفی/underflow و latch نشدن خطای commit گروهی اصلاح شدند؛ integrity، counter را با اسناد همان snapshot تطبیق می‌دهد. campaign در CTest/CI با report و log حفظ می‌شود. این تیک پذیرش bounded موتور document است؛ قطع برق، تمام مسیرهای compaction/manifest، corruption پروفایل Buffered و atomic document+graph از آن نتیجه نمی‌شوند؛ هیچ DB کاربر تغییر نکرد.

## مرحلهٔ ۲ — حافظه و کنترل منابع

- [x] **MEM-001 · P1 — گزارش مالکیت حافظه**
  - [x] counters موجود cache، memtable، graph، query، snapshot، Python و allocator را بررسی و تکمیل کن.
  - [x] مقادیر هم‌پوشان، تخمینی و غیرقابل اندازه‌گیری را صریح مشخص کن.
  - خروجی: schema telemetry و گزارش تفکیکی بدون جمع نادرست.
  - پذیرش: RSS/PSS و شمارنده‌های داخلی قابل تطبیق و شکاف attribution مستند باشد.
  - پذیرش محدود ۲۰۲۶-۱۰-۳۰: [قرارداد مالکیت و accounting](cpp/MEMORY_OWNERSHIP_FA.md) و [schema نسخهٔ ۱](telemetry/memory-v1.schema.json). native document schema ۲، GraphManager report، PMR process-wide snapshot بدون admission baseline و collector پایتون اضافه شدند. shared cache/WBM با شناسهٔ lifetime dedup می‌شوند؛ aliasها، pinned subset، reservation، peak و Python/allocator دوباره جمع نمی‌شوند. query/transaction/metadata bytes ابزارگذاری‌نشده null هستند و gap صریح است؛ total داخلیِ process عمداً null است. در ۲۲۹۲ sample، RSS partition تطبیق داشت و RSS/PSS کنار componentها ثبت شد. ۷ Python، ۵۱ native مرتبط و ۲۵ lab/native-protocol موفق؛ integration مشترک MongoDB/PostgreSQL اجرا نشد. این تیک attribution کامل، budget enforce‌شده یا پذیرش ۱۰M/۱۰۰M نیست.

- [x] **MEM-002 · P1 — حافظهٔ زنده و allocator retention**
  - وابسته به: MEM-001.
  - [x] چرخهٔ تکراری load/query/close/destroy را اندازه بگیر؛ fragmentation و cache retention را بررسی کن.
  - [x] trim را صرفاً در آزمایش تشخیصی با قبل/بعد ثبت کن؛ سیاست runtime را بر اساس نتیجه انتخاب کن.
  - خروجی: گزارش رشد، سقف و علت‌های محتمل با سطح اطمینان.
  - پذیرش: رشد زنده از حافظهٔ نگه‌داشته‌شده تفکیک شود؛ ادعای leak با شاهد باشد.
  - پذیرش محدود ۲۰۲۶-۱۰-۳۰: [روش، اعداد، سطح اطمینان و سیاست runtime](cpp/MEMORY_RETENTION_FA.md). دو workload memtable و SST/cache گرم، هرکدام سه جفت process control/trim × بیست چرخهٔ ۱۲k سند، با حذف چهار warm-up بررسی شدند؛ مجموع ۲۴۰ چرخه. sample و endpoint روی دیسک stream شدند تا خود مشاهده‌گر arena را pin نکند. PMR در همهٔ destroyها صفر شد؛ سکوهای انتهایی RSS بدون trim حدود ۹۴ و ۱۰۱MiB بودند. trim تشخیصی در SST میانهٔ حدود ۵۵MiB را با هزینهٔ حدود ۲ms آزاد کرد؛ fragmentation دقیق یا leak عمومی اثبات نشد. sampler/protocol دیگر trim خودکار ندارند؛ diagnostic opt-in حفظ شد. سقف گزارش‌شده فقط مشاهدهٔ محدود است، نه bound بلندمدت/MEM-007؛ هیچ DB کاربر دست‌کاری نشد.

- [x] **MEM-003 · P1 — reservation مشترک منابع**
  - وابسته به: MEM-001/002.
  - [x] تخصیص‌های بزرگ query، graph، buffer و snapshot را به بودجهٔ مشترک متصل کن.
  - [x] حاشیهٔ allocator، Python و charged page cache را در sizing لحاظ کن.
  - خروجی: admission/release و گزارش مصرف مؤلفه‌ها.
  - پذیرش: عبور از ظرفیت با backpressure یا خطای روشن مدیریت شود؛ budget cache معادل hard RSS معرفی نشود.
  - پذیرش محدود ۲۰۲۶-۱۰-۳۱: [قرارداد admission، sizing و مالکیت](cpp/RESOURCE_ADMISSION_SPILL_FA.md). مخزن مشترک با component report، RAII، رد صریح و انتظار محدود به query/result، staging تراکنش، buffer، graph، PMR snapshot و workspace job متصل شد. sizing پس از تنظیم مؤثر Rocks، حاشیه‌های allocator/Python/page cache/safety را کم می‌کند. اعتبار با actual allocation یا RSS جمع نمی‌شود؛ pinned Rocks versions، کپی caller و malloc دلخواه plugin gap مستند دارند. fairness عمومی و hard process cap ادعا نشده است.

- [x] **MEM-004 · P1 — buffer محدود و spill**
  - وابسته به: MEM-003 و قرارداد اجرای QUERY-001.
  - [x] اجرای scan/sort/aggregate را به batch و quota متصل کن.
  - [x] spill با مسیر خصوصی، سهمیهٔ دیسک و cleanup پس از failure بساز.
  - خروجی: عملیات بزرگ با مصرف حافظهٔ محدود.
  - پذیرش: دادهٔ موقت نشت نکند؛ قطع عملیات و پرشدن دیسک پاسخ صحیح بدهند.
  - پذیرش محدود backend در ۲۰۲۶-۱۰-۳۱: [قرارداد اجرایی و failure/recovery](cpp/RESOURCE_ADMISSION_SPILL_FA.md). API صفحه‌ای scan و external sort/COUNT/SUM با buffer و fan-in محدود، سهمیهٔ دیسک مشترک، CRC/count validation، cleanup خطا/لغو و reaper صریح پس از SIGKILL اضافه شد. SELECT/COUNT موجود quota دارند؛ grammar مربوط به ORDER BY/GROUP BY و قرارداد کامل QUERY-001/002/003 هنوز باز است. کمپین ۱۰k/۱۰۰k با quota برابر ۱MiB، ترتیب/checksum صحیح و release/cleanup کامل گذشت؛ traced peak پایتون حدود ۷۹/۹۴KiB بود، نه RSS کل process. این تیک پذیرش ۱۰M/۱۰۰M یا spill برای join نیست.

- [x] **MEM-005 · P1 — admission و fairness**
  - وابسته به: MEM-003.
  - [x] صف، concurrency و هزینهٔ query/app را محدود کن؛ maintenance سهم مشخص داشته باشد.
  - خروجی: overload policy و wait metrics.
  - پذیرش: query سنگین موجب starvation نامحدود query کوچک یا maintenance نشود.
  - پذیرش محدود ۲۰۲۶-۱۰-۳۱: [overload policy، سهم کوچک/maintenance و wait metrics](cpp/ADMISSION_SHUTDOWN_FA.md). مسیر query هر app سقف owner/queue/cost دارد؛ کار کوچک و maintenance سهم مستقل slot/cost/queue دارند. سبقت درون دسته و اولویت native job محدود شد؛ monitoring سهم مستقل دارد و migration زیر بار پیوسته فرصت chunk محدود می‌گیرد. انتظار محدود با خطای صریح است؛ هزینه تخمین policy است، نه CPU/RSS. native embedding، چند process و workerهای همهٔ مسیرها هنوز پذیرش عمومی CON-005 را ندارند.

- [x] **MEM-006 · P1 — shutdown و cancellation**
  - وابسته به: COR-002/003/007؛ برای اجرای عمومی به CON-005 متصل شود.
  - [x] ترتیب stop-admission، drain، توقف worker، release snapshot، flush و close را یکسان کن.
  - [x] safe point لغو را برای عملیات طولانی تعریف کن؛ outcome نوشتن صریح باشد.
  - خروجی: قرارداد signal/deadline و ابزار تشخیص مرحلهٔ shutdown.
  - پذیرش: تست تحت بار و خطا؛ هیچ thread kill ناامن یا موفقیت کاذب نباشد؛ مداخلهٔ cleanup ثبت شود.
  - پذیرش محدود ۲۰۲۶-۱۰-۳۱: [قرارداد signal/deadline، outcome و مراحل shutdown](cpp/ADMISSION_SHUTDOWN_FA.md). lifespan مشترک و hook زودهنگام SIGTERM/SIGINT در entrypoint عادی سرویس، token قابل انتقال به native scan/staging/snapshot/job، rollback پیش از commit و حفظ prefix commit‌شده اضافه شد. cancellation task worker را abandon نمی‌کند؛ drain timeout یا خطای durability منابع را نگه می‌دارد و failed ثبت می‌کند. نشت snapshot در exception matcher با RAII رفع شد. probe واقعی SIGTERM با cursor و تراکنش باز، release و reopen صحیح گذشت؛ ۱۳۳ document/recovery، ۸۴ graph و ۱۱۴ Python موفق، یک skip binary قدیمی. preemption commit/I/O/plugin، hook زودهنگام حالت reload و پذیرش همهٔ embeddingها ادعا نشده؛ ادامهٔ عمومی در CON-005 است.

- [ ] **MEM-007 · P1 — پذیرش حافظه در ۱۰M و ۱۰۰M**
  - وابسته به: RES-004، COR-001/002 و MEM-001/002/006.
  - [x] پروفایل موجود ۱۰M را با دو تکرار و integrity کامل بررسی کن؛ اجرا نیازمند دستور کاربر است.
  - [x] روند memory/high/max/OOM/swap، شمار snapshot و زمان close را ارزیابی کن.
  - [x] پس از تأیید، برنامهٔ جداگانهٔ ۱۰۰M تهیه کن.
  - خروجی: گزارش قبول/رد با علت و محدودهٔ اندازهٔ تأییدشده.
  - پذیرش: completed به‌تنهایی قبول نیست؛ رشد نامحدود، throttling یا OOM بدون توضیح باقی نماند؛ قبولی ۱۰M معادل ۱۰۰M نباشد.
  - پذیرش فنی محدود ۲۰۲۶-۱۰-۳۱: [گزارش قبول ۱۰M، علت و محدوده](cpp/MEMORY_ACCEPTANCE_10M_FA.md)؛ اجرای کاربر `mem007_user_20261031` با دو load مستقل ۱۰M، ۲۴ سلول و ۵۰ validation کامل پاس شد. hashهای source/config/binary تطبیق داشتند؛ تمام رخدادهای high/max/OOM، swap و full memory PSI صفر بودند؛ peak sampled charged حدود ۵٫۶۱/۵٫۶۲GiB، PSS حدود ۳٫۲۳/۳٫۱۳GiB و close حدود ۱٫۰۶/۱٫۵۵s بود. روند پایانی bounded_observed و snapshot/اعتبار پس از prepare صفر شد؛ retention allocator و relief فقط-cleanup صریح ثبت شدند. [برنامهٔ مستقل ۱۰۰M](cpp/MEMORY_100M_PLAN_FA.md) آماده است و اجرا نشده. **کارهای اجرایی این بند انجام شده‌اند؛ والد به‌علت وابستگی باز RES-004 باز می‌ماند. این قبولی ۱۰M به‌تنهایی مجوز اعلام آماده‌بودن 0.5.0 نیست.**

  - تصمیم محدودهٔ انتشار، ۲۰۲۶-۱۰-۳۱: کاربر اجرای ۱۰۰M را فعلاً کنار گذاشت و آماده‌سازی 0.5.0 را بر مبنای قبولی ۱۰M خواست. [candidate آزمایشی محدود 0.5.0](releases/v0.5.0.md) محلی ساخته و نصب تمیز شد؛ RES-004/005 و پذیرش ۱۰۰M همچنان باز هستند و پاس‌شده معرفی نمی‌شوند. آماده‌سازی این بسته گیت تحقیقاتی والد را نمی‌بندد؛ انتشار عمومی انجام نشده است.

## مرحلهٔ ۳ — خواندن و planner

- [ ] **READ-001 · P1 — profiler مسیر خواندن**
  - وابسته به: RES-004/005 و MEM-001.
  - [ ] lock wait، metadata، planning، index seek/next، fetch، predicate، decode و Python conversion را تفکیک کن.
  - [ ] نمونه‌برداری محدود و امکان خاموش‌کردن instrumentation فراهم کن.
  - خروجی: breakdown و overhead روشن/خاموش.
  - پذیرش: زمان‌ها تعریف بدون دوباره‌شماری داشته باشند؛ دادهٔ حساس ثبت نشود.

- [ ] **READ-002 · P1 — lookup متناسب با نتیجه**
  - وابسته به: READ-001 و COR-004/006.
  - [ ] batch ثابت MultiGet را با limit، selectivity و predicate باقی‌مانده تطبیق بده.
  - [ ] مسیر سبک equality کم‌خروجی را با مسیر عمومی مقایسه کن.
  - خروجی: کاهش اسناد/کلیدهای واکشی‌شده در lookup کوچک.
  - پذیرش: کار lookup یکتا با رشد اندازهٔ dataset به scan تبدیل نشود؛ نتیجه با oracle برابر بماند.

- [ ] **READ-003 · P1 — حذف تبدیل‌های تکراری**
  - وابسته به: READ-001 و COR-004.
  - [ ] BSON→JSON→Python را با خروجی BSON یا ساخت مستقیم مقدار مقایسه کن.
  - [ ] bytes copied، allocation، ownership و عمر buffer را ثبت کن.
  - خروجی: مسیر انتخاب‌شده بر اساس پروفایل با API سازگار یا versioned.
  - پذیرش: type/Unicode/null صحیح؛ تست GIL و lifetime؛ gain همراه overhead گزارش شود.

- [ ] **READ-004 · P1 — projection و predicate pushdown**
  - وابسته به: READ-001/003 و COR-004/006.
  - [ ] decode و انتقال فیلد/سند غیرضروری را کاهش بده.
  - خروجی: اجرای native برای subset مشخص projection/predicate.
  - پذیرش: nested path و missing/null با مرجع برابر؛ bytes و CPU قبل/بعد ثبت شوند.

- [ ] **READ-005 · P1 — planner مبتنی بر آمار**
  - وابسته به: READ-001 و CORE-001 برای version و invalidation metadata.
  - [ ] cardinality، distinct count، selectivity و version آمار را تعریف کن.
  - [ ] score ثابت را مرحله‌ای با انتخاب هزینه‌محور جایگزین کن.
  - خروجی: آمار قابل refresh و plan قابل توضیح.
  - پذیرش: دادهٔ skewed، indexهای رقیب و stale statistics تست شوند؛ آمار روی صحت نتیجه اثر نگذارد.

- [ ] **READ-006 · P1 — plan cache نسخه‌دار**
  - وابسته به: READ-005 و CORE-001.
  - [ ] کلید cache، generation، invalidation و سقف حافظه را تعریف کن.
  - خروجی: cache قابل خاموش‌کردن با hit/miss/eviction metrics.
  - پذیرش: تغییر schema/index و drop/recreate plan قدیمی را معتبر نکند؛ پارامتر/مجوز کاربر نشت نکند.

- [ ] **READ-007 · P2 — ایندکس‌های موردنیاز کاربرد**
  - وابسته به: RES-001 و COR-004/005/007.
  - [ ] وضعیت compound فعلی را اثبات کن؛ covering، partial و multikey را بر اساس نیاز اولویت‌بندی کن.
  - [ ] برای هر نوع، mutation، unique، rebuild و recovery را جدا اجرا کن.
  - خروجی: index capability matrix و هزینهٔ write/storage.
  - پذیرش: scan=index برای corpus مرتبط؛ feature ناقص صریح رد شود.

- [ ] **READ-008 · P1 — Explain/Analyze و regression history**
  - وابسته به: READ-001/005.
  - [ ] estimated/actual rows، bytes، زمان، lock wait و plan signature ثبت کن.
  - [ ] تاریخچهٔ محدود و قابل تنظیم برای تغییر عملکرد بساز.
  - خروجی: گزارش قابل استفاده در CLI/dashboard.
  - پذیرش: Explain بدون اجرای ناخواستهٔ write؛ history بدون literal حساس و با مصرف محدود باشد.

## مرحلهٔ ۴ — هم‌زمانی و Python/C++

- [ ] **CON-001 · P1 — نقشهٔ contention**
  - وابسته به: READ-001 و MEM-007 در اندازهٔ آزمایش.
  - [ ] mutexهای metrics، DDL، metadata، registry و storage را با انتظار واقعی رتبه‌بندی کن.
  - خروجی: پروفایل ۱/۱۰/۵۰ reader و تفکیک CPU، I/O و memory pressure.
  - پذیرش: علت افت با افزایش client با شاهد مشخص شود؛ فرضیه به‌عنوان نتیجه نوشته نشود.

- [ ] **CON-002 · P1 — کاهش قفل مشترک خواندن**
  - وابسته به: CON-001 و COR-006.
  - [ ] فقط قفل‌های اثبات‌شدهٔ گلوگاه را اصلاح کن؛ metrics thread-local/aggregated در صورت توجیه بررسی شود.
  - خروجی: تغییر محدود با قرارداد lock order و ownership.
  - پذیرش: race/deadlock/lifetime regression پاس؛ throughput و p99 در منابع ثابت بهتر یا trade-off مستند باشد.

- [ ] **CON-003 · P0/P1 — ممیزی GIL همهٔ bindingها**
  - [ ] برای هر binding، دسترسی Python و بخش طولانی native را فهرست کن.
  - [ ] release/reacquire، exception و callback را با قرارداد یکسان اصلاح کن.
  - خروجی: جدول GIL و تست subprocess/threading.
  - پذیرش: دسترسی بدون GIL به Python یا release تکراری رخ ندهد؛ no-GIL بودن معادل thread-safe اعلام نشود.

- [ ] **CON-004 · P1 — batch و buffer ownership**
  - وابسته به: CON-003، COR-004/007 و READ-003.
  - [ ] APIهای list، packed buffer و BSON را از نظر copy و CPU مقایسه کن.
  - [ ] ورودی mutable، buffer قرض‌گرفته و خروجی owned را دقیق تعریف کن.
  - خروجی: مسیر کم‌هزینه با سقف بایت و تعداد.
  - پذیرش: mutation هم‌زمان ورودی، خطا و lifetime دادهٔ خراب نسازند؛ zero-copy فقط برای بخش واقعاً بدون کپی ادعا شود.

- [ ] **CON-005 · P1 — worker و صف محدود**
  - وابسته به: MEM-003/005، CON-003 و COR-005.
  - [ ] قرارداد executor، تعداد worker، صف، deadline و shutdown را تعریف کن.
  - خروجی: مدیریت concurrent query و cancel token قابل انتقال به native.
  - پذیرش: oversubscription، starvation و لغو درخواست رفتار قابل پیش‌بینی داشته باشند.
  - زیرساخت محدود ۲۰۲۶-۱۰-۳۱ در [MEM-005/006](cpp/ADMISSION_SHUTDOWN_FA.md): صف و سهم app/query، deadline و native cancel token برای سرویس و scheduler موجود آماده است؛ audit تعداد workerهای همهٔ entrypointها، embedding/plugin و پذیرش عمومی همچنان باز است.

- [ ] **CON-006 · P1 — آزمایش مقیاس‌پذیری**
  - وابسته به: CON-001..005 و RES-004.
  - [ ] ۱/۲/۴/۸/۱۶/۳۲/۵۰ client را با بار read/write/mixed بررسی کن.
  - خروجی: منحنی throughput/latency و CPU/lock/I/O برای هر اندازه.
  - پذیرش: نقطهٔ اشباع توضیح‌پذیر و تکرارپذیر باشد؛ رشد خطی تضمین نشود.

## مرحلهٔ ۵ — نوشتن و WAL

- [ ] **WRITE-001 · P1 — breakdown مسیر mutation**
  - وابسته به: RES-004/005 و COR-003.
  - [ ] validation، metadata، lookup، index، lock، WAL و sync را جدا اندازه بگیر.
  - خروجی: پروفایل single/bulk/concurrent و هزینهٔ ابزار.
  - پذیرش: ACK wait با CPU generation یا compaction به‌اشتباه یکی نشود.
  - پیشرفت ۲۰۲۶-۱۰-۲۹: [telemetry و تشخیص Durable](cpp/DURABLE_GROUP_COMMIT_FA.md)؛ زمان commit/counter lookup/DDL wait و ۲۴ cell کوتاه ثبت شد. تفکیک کامل validation/index/compaction و هزینهٔ ابزار هنوز کامل نیست؛ والد باز می‌ماند.

- [ ] **WRITE-002 · P1 — group commit تراکنشی**
  - وابسته به: WRITE-001، COR-003/005/007 و CON-001.
  - [x] ابتدا grouping موجود RocksDB را زیر چند نویسنده بررسی کن.
  - [ ] فقط در صورت نیاز batching بالاتر با ترتیب commit روشن پیاده کن.
  - خروجی: بهبود مسیر عادی writer، مستقل از exclusive ingestion.
  - پذیرش: ACK مطابق durability؛ crash پس از ACK، atomicity سند/index/counter و p99 تست شوند.
  - شواهد جزئی ۲۰۲۶-۱۰-۲۹: [grouping واقعی و helper اختیاری](cpp/DURABLE_GROUP_COMMIT_FA.md)؛ در ۱۰ writer با collectionهای مستقل حدود ۵ commit/sync و در collection مشترک حدود ۱ مشاهده شد. صف bounded درج native و ۱۲ crash/reopen پاس شدند؛ شمارندهٔ مشترک حذف نشد. مسیر عمومی transaction، وابستگی‌ها، sweep تأخیر و آزمون پایدار بلند هنوز کامل نیستند؛ والد و زیرتسک batching عمومی باز می‌مانند.

- [ ] **WRITE-003 · P1 — کاهش کار تکراری mutation**
  - وابسته به: WRITE-001 و COR-004..007.
  - [ ] metadata lookup، encoding و تغییر index بدون تغییر مقدار را پروفایل و بهینه کن.
  - خروجی: کاهش bytes/op و allocation با قرارداد cache invalidation.
  - پذیرش: schema/FK/unique bypass نشوند؛ rollback و index consistency پاس باشند.

- [ ] **WRITE-004 · P1 — amplification و compaction**
  - وابسته به: WRITE-001 و MEM-003.
  - [ ] WAL/flush/compaction bytes، read amplification، debt و stalls را ثبت کن.
  - [ ] تنظیم‌ها را با RAM و durability ثابت A/B کن.
  - خروجی: سیاست تنظیم بر اساس workload و trade-off مستند.
  - پذیرش: بهبود کوتاه‌مدت با بدهی compaction یا افزایش بی‌گزارش RAM پنهان نشود.

- [ ] **WRITE-005 · P1 — ingestion همراه query**
  - وابسته به: WRITE-002/004، MEM-005 و CON-006.
  - [ ] تأثیر batch/exclusive gate بر query کوچک و p99 را اندازه بگیر.
  - خروجی: سیاست admission و profileهای ingest/mixed روشن.
  - پذیرش: bulk موجب starvation نامحدود نشود؛ ظرفیت bulk به ترافیک عادی تعمیم داده نشود.

- [ ] **WRITE-006 · P1 — مطالعهٔ رشد تا ۱۰۰M**
  - وابسته به: MEM-007، WRITE-001/004 و RES-004.
  - [ ] نقاط ۱M/۵M/۱۰M و سپس با اجازهٔ کاربر ۱۰۰M را ثبت کن.
  - [ ] نرخ پنجره‌ای، ACK، index size، cache miss، WAL و compaction را به زمان وصل کن.
  - خروجی: تحلیل علت افت با شواهد و آزمایش کنترل‌شده.
  - پذیرش: میانگین و بدترین پنجره‌ها گزارش شوند؛ ۱۵۰k هدف است تا در کل دامنه اثبات شود.

- [ ] **WRITE-007 · P2 — پروفایل سخت‌افزار و منابع**
  - وابسته به: WRITE-004/006.
  - [ ] batch/buffer/cache را برای دیسک و RAM مشخص و با سقف ثابت ارزیابی کن.
  - خروجی: مجموعهٔ config versioned و محدودهٔ اعتبار هرکدام.
  - پذیرش: تنظیم مناسب یک سخت‌افزار بهبود عمومی نامیده نشود؛ startup تنظیم نامعتبر را رد کند.

## مرحلهٔ ۶ — صحت و مزیت document+graph

- [ ] **GRAPH-001 · P0 — outbox اتمیک هسته**
  - وابسته به: COR-003/005/007.
  - [ ] event را با document/index/counter در یک commit ذخیره کن؛ تمام مسیرهای native/API/bulk/transaction پوشش یابند.
  - خروجی: log نسخه‌دار با post-image یا اطلاعات بازسازی کافی.
  - پذیرش: commit بدون event و event برای rollback وجود نداشته باشد؛ replay idempotent باشد.

- [ ] **GRAPH-002 · P0 — اجرای واقعی mapping**
  - وابسته به: COR-004 و GRAPH-001.
  - [ ] filter، property، defaults و ID واقعی را با evaluator مشترک در build/update اعمال کن.
  - خروجی: semantics mapping و خطای زودهنگام قابلیت unsupported.
  - پذیرش: سند خارج از filter وارد graph نشود؛ post-image با سند واقعی یکسان باشد.

- [ ] **GRAPH-003 · P0 — هویت node و مالکیت edge**
  - وابسته به: GRAPH-001/002.
  - [ ] remove/re-add برای property update را با رفتار صحیح هویت جایگزین کن.
  - [ ] UNWIND، shrink/reorder و چند منبع مشترک رابطه را مدیریت کن.
  - خروجی: diff و ownership/reference contract.
  - پذیرش: رابطهٔ نامرتبط حذف نشود؛ incremental=reference rebuild در update/delete/retry.

- [ ] **GRAPH-004 · P0 — rebuild با انتشار اتمیک**
  - وابسته به: GRAPH-001..003 و COR-007.
  - [ ] generation تازه بساز و نسخهٔ فعال را تا انتشار معتبر نگه دار.
  - خروجی: build/cancel/recovery قابل مشاهده و بدون پاک‌کردن زودهنگام نسخهٔ معتبر.
  - پذیرش: failure هر مرحله old معتبر یا new کامل باقی بگذارد.

- [ ] **GRAPH-005 · P0 — reconciliation و lineage**
  - وابسته به: GRAPH-001/004 و CORE-001.
  - [ ] UUID منبع، mapping version و source/applied sequence را ذخیره و مقایسه کن.
  - خروجی: startup/readiness و replay/rebuild policy.
  - پذیرش: graph عقب‌مانده، DB دیگر و retention gap تشخیص داده شوند؛ healthy کاذب نباشد.

- [ ] **GRAPH-006 · P1 — consistency query ترکیبی**
  - وابسته به: GRAPH-004/005 و COR-005.
  - [ ] cut سازگار یا lag contract و wait policy را تعریف کن.
  - خروجی: query document+graph با consistency قابل انتخاب و قابل مشاهده.
  - پذیرش: زیر writer، نتیجه مطابق قرارداد باشد؛ ترکیب خاموش دو زمان متفاوت رخ ندهد.

- [ ] **GRAPH-007 · P1 — oracle الگوریتم‌ها**
  - وابسته به: GRAPH-003.
  - [ ] traversal/centrality/community را برای directed، disconnected، multi-edge، self-loop و deletion تست کن.
  - خروجی: مرجع مستقل، seed و قرارداد approximation/weight.
  - پذیرش: خطا یا اختلاف بیش از tolerance ثبت و رفع شود؛ نام الگوریتم جای شاهد صحت نباشد.

- [ ] **GRAPH-008 · P1 — benchmark کاربرد ترکیبی**
  - وابسته به: GRAPH-006/007 و RES-001..005.
  - [ ] هزینهٔ query، update، sync و rebuild را با مرجع مناسب اندازه بگیر.
  - خروجی: فرضیهٔ مزیت document+graph با dataset و آزمایش حذف مؤلفه.
  - پذیرش: منابع کل و lag حساب شوند؛ سود query با هزینهٔ پنهان همگام‌سازی جبران نشده باشد.

## مرحلهٔ ۷ — query، catalog و تجربهٔ توسعه

- [ ] **QUERY-001 · P1 — مشخصات اجرایی زبان**
  - وابسته به: COR-004/006.
  - [ ] subset زبان، type/error semantics و syntax unsupported را از corpus واحد تعریف کن.
  - خروجی: contract اجراشدنی برای parser/evaluator/API.
  - پذیرش: syntax پذیرفته‌شده تفسیر بی‌صدای متفاوت نداشته باشد.

- [ ] **QUERY-002 · P1 — ORDER BY و LIMIT پایدار**
  - وابسته به: QUERY-001، COR-002 و MEM-004.
  - [ ] tie-break/collation و مسیر index-order یا top-k محدود را اجرا کن.
  - خروجی: pagination و sort deterministic.
  - پذیرش: duplicate sort key، null و تغییر هم‌زمان داده طبق snapshot contract تست شوند.

- [ ] **QUERY-003 · P1 — JOIN با cardinality صحیح**
  - وابسته به: QUERY-001، COR-005 و MEM-004.
  - [ ] first-match فعلی را برای subset منتخب به semantics صریح JOIN تبدیل کن یا محدودیت را رد کن.
  - خروجی: one-to-many، duplicate/no-match و snapshot مشترک.
  - پذیرش: corpus با oracle رابطه‌ای برابر باشد؛ این تسک با cursor انتقال JOIN یکی نیست.

- [ ] **QUERY-004 · P2 — aggregation نوع‌دار**
  - وابسته به: QUERY-001، COR-004 و MEM-004.
  - [ ] GROUP BY/HAVING و توابع منتخب کاربرد را با overflow/decimal/null اجرا کن.
  - خروجی: aggregation تدریجی و spillable برای subset مشخص.
  - پذیرش: ورودی خالی، عدد مرزی و دادهٔ بزرگ با مرجع برابر باشند.

- [ ] **QUERY-005 · P2 — نوع‌های دادهٔ کاربردی**
  - وابسته به: COR-004 و QUERY-001.
  - [ ] زمان، decimal، UUID و binary را بر اساس نیاز تعریف و version کن.
  - خروجی: codec/index/API یکسان همراه migration لازم.
  - پذیرش: precision، ordering، timezone و roundtrip در تمام مسیرهای پشتیبانی‌شده صحیح باشند.

- [ ] **CORE-001 · P1 — catalog پایدار و dependency**
  - [ ] ID و generation برای database/collection/index/schema/graph تعریف کن؛ وابستگی‌ها را ثبت کن.
  - خروجی: metadata versioned و قرارداد rename/drop/recreate.
  - پذیرش: object هم‌نام جدید به plan/mapping قدیمی وصل نشود؛ crash catalog تست شود.

- [ ] **CORE-002 · P1 — DDL و schema evolution قابل بازیابی**
  - وابسته به: CORE-001، COR-005/007 و MEM-003.
  - [ ] lifecycle build/cancel/resume/drop index و validation دادهٔ قبلی در schema change را تکمیل کن.
  - خروجی: progress، disk budget و publication قرارداددار.
  - پذیرش: index ناقص Ready نشود؛ unique build race، concurrent mutation و restart تست شوند.

- [ ] **DX-001 · P2 — driver و protocol نسخه‌دار**
  - وابسته به: QUERY-001 و COR-003/005؛ cancellation به CON-005 وابسته است.
  - [ ] capability negotiation، خطا، retry، timeout، session و sync/async را تعریف کن.
  - [ ] thread wrapper فعلی async را پروفایل کن؛ transport غیرمسدودکننده را در صورت توجیه اضافه کن.
  - خروجی: conformance مشترک؛ زبان‌های جدید طبق نیاز RES-001.
  - پذیرش: outcome و type بین driverها برابر؛ fork/event-loop sharing نامعتبر رد شود.

- [ ] **DX-002 · P2 — import/export قابل ادامه**
  - وابسته به: COR-001/002، QUERY-005 و CORE-001.
  - [ ] checksum، checkpoint، dedup، quarantine و گزارش type loss بساز.
  - خروجی: انتقال subset مشخص از Mongo/PostgreSQL/graph و export محدود از نظر حافظه.
  - پذیرش: قطع و ادامه موجب گم‌شدن یا دوباره‌اعمال‌کردن خاموش داده نشود.

- [ ] **DX-003 · P1 — dashboard مبتنی بر وضعیت واقعی**
  - وابسته به: MEM-001، READ-008 و GRAPH-005 برای نمایش graph lag.
  - [ ] plan، memory، pressure، WAL، queue، lag و مرحلهٔ عملیات را نشان بده.
  - خروجی: UI همخوان قرارداد API و effective settings.
  - پذیرش: قابلیت ناقص ready نشان داده نشود؛ error/cancel/unknown outcome واضح باشند.

## مرحلهٔ ۸ — امنیت پژوهشی و قابل ارزیابی

- [ ] **SEC-001 · P0 — threat model**
  - [ ] دارایی و مرز اعتماد OS owner، embedded، API، admin، backup و dependency را تعریف کن.
  - خروجی: تهدید، کنترل و آزمون متناظر؛ محدودهٔ ادعای امنیت.
  - پذیرش: مهاجم و دسترسی‌های فرض‌شده روشن باشند؛ امنیت مطلق ادعا نشود.

- [ ] **SEC-002 · P0 — احراز هویت و ابطال session**
  - وابسته به: SEC-001.
  - [ ] disable، تغییر role/password، revoke، logout و bootstrap را در HTTP/Socket بررسی کن.
  - خروجی: session version یا پروتکل ابطال معتبر.
  - پذیرش: token قبلی دسترسی نامعتبر ندهد؛ نصب اولیه تصاحب عمومی نشود.

- [ ] **SEC-003 · P0 — بودجهٔ عملیات پرهزینه**
  - وابسته به: SEC-001 و MEM-003/005؛ لغو native به CON-005 متصل شود.
  - [ ] Argon2 concurrency، regex، AST/input و graph algorithm را محدود کن.
  - خروجی: rate/admission/deadline و regex با تضمین منابع مناسب.
  - پذیرش: ورودی adversarial باعث OOM، ReDoS یا starvation نامحدود نشود؛ hash امنیتی ضعیف نشود.

- [ ] **SEC-004 · P0 — پوشش authorization**
  - وابسته به: SEC-001/002.
  - [ ] endpointها و مسیرهای bulk/stream/transaction/graph/maintenance را با نقش‌ها ممیزی کن.
  - خروجی: ماتریس دسترسی و تست end-to-end، شامل سطح مجوزهایی که واقعاً پشتیبانی می‌شوند.
  - پذیرش: bypass بین مسیرها نباشد؛ مجوز پشتیبانی‌نشده بی‌صدا پذیرفته نشود.

- [ ] **SEC-005 · P0/P1 — امنیت حافظه و fuzzing native**
  - وابسته به: SEC-001؛ روی COR-006/007 بنا شود.
  - [ ] BSON/parser/index/protocol/recovery را با corpus و sanitizer پوشش بده.
  - خروجی: targetهای fuzz، سقف منابع، artifact failure و regression.
  - پذیرش: crash و یافتهٔ sanitizer قابل دسترس حل یا blocker ثبت شوند؛ fuzz بدون شاهد سلامت کامل اعلام نشود.

- [ ] **SEC-006 · P1 — secret و audit**
  - وابسته به: SEC-001/002 و CORE-001 در صورت versioning کلید.
  - [ ] نمایش یک‌باره، rotation، redaction و audit actor/action/result را تکمیل کن.
  - خروجی: key lifecycle و log با retention/quota و سیاست full disk.
  - پذیرش: listing/log/diagnostic secret کامل ندهند؛ تغییر امنیت قابل پیگیری باشد.

- [ ] **SEC-007 · P1 — داده و backup رمز‌شده**
  - وابسته به: SEC-001/006 و REC-001؛ طراحی اولیه می‌تواند زودتر انجام شود.
  - [ ] راهکار منتخب برای data/WAL/spill/backup و مدیریت کلید را مشخص کن.
  - خروجی: threat coverage، rotation و recovery policy.
  - پذیرش: restore، key loss، permission و عدم نشت plaintext در محدودهٔ راهکار تست شوند.

- [ ] **SEC-008 · P1 — ارزیابی مستقل امنیت**
  - وابسته به: SEC-001..007 برای محدودهٔ نسخهٔ ارزیابی.
  - [ ] بستهٔ reproducible test و یافته‌ها را آماده کن و بازبینی انسانی مستقل بگیر.
  - خروجی: گزارش نسخه‌دار، رفع یافته‌ها و فرایند گزارش آسیب‌پذیری.
  - پذیرش: AI نمی‌تواند تأیید مستقل انسانی را به‌جای فرد واقعی تیک بزند؛ ریسک باز صریح بماند.

## مرحلهٔ ۹ — recovery و تحقیق چندنودی

- [ ] **REC-001 · P1 — backup/restore سازگار**
  - وابسته به: COR-003/007، CORE-001 و GRAPH-005؛ receipt اتمیک در CORE-003.
  - [ ] document، graph، catalog و receipt را در یک محدودهٔ بازیابی معتبر نگه دار.
  - خروجی: backup manifest و restore روی مقصد تازه؛ سیاست offline/online روشن.
  - پذیرش: query و شمارش/digest معتبر؛ backup ناقص/ناسازگار رد شود؛ فایل backup به‌تنهایی شاهد نباشد.

- [ ] **CORE-003 · P0 — receipt اتمیک درخواست**
  - وابسته به: COR-003/005/007.
  - [ ] receipt و اثر mutation را در یک commit domain قرار بده؛ reconciliation برای پاسخ گم‌شده بساز.
  - خروجی: retry/idempotency contract همراه retention و unknown outcome.
  - پذیرش: crash بین commit و پاسخ اثر دوباره نسازد؛ receipt فعال/نامعلوم بی‌قاعده حذف نشود.

- [ ] **REC-002 · P2 — point-in-time recovery**
  - وابسته به: REC-001، GRAPH-001/005 و CORE-001/003.
  - [ ] base backup، log archive، lineage و انتخاب sequence/زمان را طراحی و اجرا کن.
  - خروجی: ابزار PITR با گزارش RPO/RTO اندازه‌گیری‌شده.
  - پذیرش: gap و log متعلق به DB دیگر رد شوند؛ projection به state معتبر برسد.

- [ ] **DIST-001 · P3 — specification replication**
  - وابسته به: COR-005/007، CORE-001/003 و GRAPH-001.
  - [ ] commit identity، ترتیب log، consistency، membership و ACK sync/async را پیش از prototype تعریف کن.
  - خروجی: specification و failure model قابل بازبینی.
  - پذیرش: WAL محلی با consensus اشتباه نشود؛ invariants و تست history مشخص باشند.

- [ ] **DIST-002 · P3 — follower و catch-up**
  - وابسته به: DIST-001 و REC-001/002.
  - [ ] initial snapshot، log replay، lag، retention و backpressure را پیاده کن.
  - خروجی: replica با read policy مشخص و telemetry.
  - پذیرش: gap/corruption/restart/stale read مطابق قرارداد مدیریت شوند.

- [ ] **DIST-003 · P3 — failover و fencing**
  - وابسته به: DIST-001/002.
  - [ ] election، epoch/fencing و بازگشت عضو قدیمی را طراحی کن.
  - خروجی: history آزمون partition/pause/rejoin و راهنمای recovery.
  - پذیرش: دو writer معتبر ایجاد نشوند؛ دادهٔ تأییدشده مطابق RPO اعلام‌شده حفظ شود.

- [ ] **DIST-004 · P3 — sharding پژوهشی**
  - وابسته به: DIST-003 و RES-001.
  - [ ] shard key، rebalance، hotspot و cross-shard semantics را ابتدا برای document تعریف کن.
  - [ ] اثر partition بر graph، join، constraint و transaction را جدا ارزیابی کن.
  - خروجی: prototype و محدودهٔ دقیق قابلیت.
  - پذیرش: انتقال shard داده را گم نکند یا دو authoritative owner نسازد؛ unsupported عملیات رد شود.

## مرحلهٔ ۱۰ — شواهد پژوهش و ارائهٔ متن‌باز

- [ ] **EVAL-001 · P1 — ماتریس نهایی آزمایش**
  - وابسته به: RES-001..005 و تسک‌های مسیرهای مورد ادعا.
  - [ ] دادهٔ کوچک/بزرگ‌تر از RAM، cache condition قابل اثبات، read/write/mixed و concurrency را پوشش بده.
  - خروجی: برنامه و دادهٔ خام آزمایش؛ cold فقط اگر واقعاً تأیید شده باشد.
  - پذیرش: correctness/durability/resources و scope هر مقایسه روشن باشند.

- [ ] **EVAL-002 · P1 — آزمایش حذف مؤلفه‌ها**
  - وابسته به: EVAL-001 و بهینه‌سازی‌های مورد ادعا.
  - [ ] هر تغییر مهم را جدا روشن/خاموش یا با نسخهٔ پایه مقایسه کن.
  - خروجی: سهم مستقل profiler/cache/index/batch/locking و تعامل آن‌ها.
  - پذیرش: gain به مجموعهٔ تغییرات هم‌زمان نسبت داده نشود؛ overhead و نتیجهٔ منفی حفظ شوند.

- [ ] **EVAL-003 · P1 — تحلیل آماری و نمودار**
  - وابسته به: EVAL-001/002.
  - [ ] تکرارها، CI مناسب، outlierها، throughput و p50/p95/p99 را گزارش کن.
  - خروجی: دادهٔ خام و اسکریپت تولید نمودار مستقل و قابل اجرا.
  - پذیرش: بهترین run گزینش نشود؛ روش percentile و واحد منابع روشن باشد؛ نمودارها به نسخه متصل باشند.

- [ ] **EVAL-004 · P1 — بازتولید مستقل**
  - وابسته به: EVAL-003 و OSS-001.
  - [ ] بستهٔ آزمایش را روی میزبان دوم با مشخصات ثبت‌شده اجراپذیر کن.
  - [ ] اجرای فرد مستقل و اختلاف نتایج را ثبت کن.
  - خروجی: گزارش reproduction و محدودهٔ اعتبار نتیجه.
  - پذیرش: شخص دیگر بدون تغییر دستی هسته بتواند اجرا کند؛ AI جای تأیید فرد مستقل امضا نکند.

- [ ] **OSS-001 · P1 — build و نصب بازتولیدپذیر**
  - [ ] dependency و toolchain را pin و CI نصب تمیز artifact را اجرا کن.
  - [ ] platform/Python/ABI پشتیبانی‌شده، SBOM و upgrade fixture را ثبت کن.
  - خروجی: دستور build/install/test معتبر و artifact قابل ردیابی.
  - پذیرش: نمونهٔ نصب از محیط تمیز به native واقعی برسد؛ پلتفرم آزمایش‌نشده پشتیبانی‌شده نامیده نشود.

- [ ] **OSS-002 · P1 — راهنمای مشارکت**
  - وابسته به: OSS-001 و قراردادهای مرتبط COR/QUERY.
  - [ ] معماری، تست، style، issue/PR template و good-first-issue بساز.
  - خروجی: مسیر onboarding و نقشهٔ مالکیت ماژول‌ها.
  - پذیرش: مشارکت‌کننده بتواند یک تغییر کوچک را build/test کند؛ مستندات مثال اجراشدنی داشته باشند.

- [ ] **OSS-003 · P1 — همخوانی مجوزها**
  - [ ] LICENSE، README، package metadata و third-party notices را بررسی کن.
  - خروجی: تناقض‌ها و تصمیم مجوز مالک پروژه؛ اصلاح فایل‌ها پس از تصمیم لازم.
  - پذیرش: AI مجوز را خودسرانه عوض نکند؛ artifactها با تصمیم ثبت‌شده همخوان باشند.

- [ ] **DEMO-001 · P1 — سناریوی زندهٔ نمایشگاه**
  - وابسته به: نسخهٔ منتخب تسک‌ها، DX-003، REC-001 و EVAL-003.
  - [ ] load، query ترکیبی، Explain، memory limit و recovery را با دادهٔ عمومی/مجاز آماده کن.
  - [ ] reset و اجرای دوباره را بدون حذف مبهم دادهٔ کاربر فراهم کن.
  - خروجی: دستور demo، داده، زمان تقریبی و ویدئوی پشتیبان.
  - پذیرش: demo از نصب تمیز قابل اجرا و ادعاهایش هم‌سطح شواهد باشند.

- [ ] **DEMO-002 · P1 — گزارش فنی پژوهش**
  - وابسته به: RES-003 و EVAL-003/004.
  - [ ] مسئله، روش‌های موجود، طراحی، سهم ایده، آزمایش و نتایج منفی را بنویس.
  - خروجی: گزارش فارسی و خلاصهٔ انگلیسی همراه منابع و محدودیت‌ها.
  - پذیرش: نوآوری به‌طور مشخص از مهندسی مجدد تفکیک شود؛ ادعای برتری دامنه و شاهد داشته باشد.

- [ ] **DEMO-003 · P1 — نسخهٔ قابل ارائه**
  - وابسته به: OSS-001..003 و DEMO-001/002؛ برای ادعاهای مستقل EVAL-004 و SEC-008 مرتبط بررسی شوند.
  - [ ] tag/artifact، config، dataset و گزارش‌ها را در manifest نسخهٔ نمایشگاه متصل کن.
  - [ ] known issues، قابلیت‌های مجاز و محدودیت‌های demo را منتشر کن.
  - خروجی: بستهٔ نمایشگاه و راهنمای بازتولید عمومی.
  - پذیرش: ارائه‌کننده برای هر ادعای روی پوستر بتواند شاهد همان نسخه را نشان دهد؛ نسخهٔ جدید خودکار شواهد قبلی را به ارث نبرد.

## دروازه‌های عبور

- [ ] **GATE-A — پایهٔ معتبر:** قراردادهای COR-001..007 مرتبط با قابلیت ارائه‌شده پاس؛ artifact و baseline قابل ردیابی.
- [ ] **GATE-B — حافظه و shutdown:** MEM-007 در اندازهٔ اعلام‌شده تأیید؛ pressure، رشد و مداخلهٔ cleanup پنهان نباشند.
- [ ] **GATE-C — مزیت عملکردی:** EVAL-001..003 برای حداقل یک فرضیهٔ RES-003، با صحت و تضمین برابر تأیید شود.
- [ ] **GATE-D — document+graph:** برای ادعای همگامی، GRAPH-001..006 با crash و replay پاس باشند.
- [ ] **GATE-E — امنیت محدودهٔ ارائه:** تهدیدهای قابل دسترس SEC-001 و آزمون‌های مرتبط پوشش داشته باشند؛ یافتهٔ باز اعلام شود.
- [ ] **GATE-F — نمایشگاه تک‌نودی:** GATE-A/B/E، دست‌کم یک مزیت GATE-C، و DEMO-001..003 آماده باشند؛ برای demo ترکیبی GATE-D نیز لازم است.
- [ ] **GATE-G — ادعای چندنودی:** DIST-001..003 و recovery همان topology تأیید شوند؛ برای نمایشگاه تک‌نودی اختیاری است.

بسته‌شدن تسک‌ها پایان تحقیق نیست. هر نتیجه فقط برای نسخه، workload، محیط و تضمین آزموده‌شده اعتبار دارد. قابلیت‌های اختیاری مانند sharding یا نوع index خاص می‌توانند در نسخهٔ نمایشگاه خارج از محدوده بمانند؛ مسیر unsupported باید صریح باشد.
