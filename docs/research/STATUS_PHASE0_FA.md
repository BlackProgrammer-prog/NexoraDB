# وضعیت مرحلهٔ صفر — ۲۰۲۶-۱۰-۲۸

این بخش گزارش اولیهٔ شروع کار است؛ وضعیت به‌روز و تیک‌های پذیرفته‌شده در بخش «پیگیری و پذیرش فنی ۲۰۲۶-۱۰-۲۸» پایین همین فایل آمده است. وضعیت کل مرحله هنوز **در حال اجرا** است.
هیچ commit، نصب محصول جدید، تغییر سرویس یا اجرای ۱۰M/۱۰۰M انجام نشد.

## خروجی‌ها و ادامهٔ دقیق

| تسک | انجام‌شده | باقی‌مانده برای پذیرش |
| --- | --- | --- |
| RES-001 | سند versioned سه سناریو، قرارداد داده/query/oracle؛ smoke هر سه روی native | concurrency و اجرای resource-constrained واقعی W3؛ pilot در حجم هدف |
| RES-002 | ماتریس پنج رقیب، edition/family، semantics و منابع رسمی؛ N/A و pending صریح | patch/digest واقعی، adapters و effective config، بررسی شرایط انتشار |
| RES-003 | شش فرضیهٔ محدود با controls و آستانهٔ پیشنهادی | freeze نسخهٔ baseline مستقل و preregistration نهایی پس از ماتریس |
| RES-004 | ابزار plan/run/analyze، order seedدار ABA/BAB، hash سورس/binary، evidence اختصاصی، drift detection | CI آماری، host/cgroup telemetry، تطبیق artifact اجراشده با declared binary، A/B واقعی |
| RES-005 | ممیزی ابزارهای موجود و قرارداد L0/L1/L2؛ جلوگیری از double count در برچسب scope | driverهای matched C++/binding/network و sampler مستقل؛ عدد breakdown هنوز نداریم |

## شواهد اجرا

فرمان:

```bash
NEXORA_RESEARCH_NATIVE=1 python3 -m unittest discover -s tests -p test_research_phase0.py -v
```

نتیجه: **۹ تست پاس**؛ شامل سه native scenario، oracle مستقل graph، plan قطعی، false/NaN rejection، drift، دستکاری شواهد، عدم overwrite و timeout cleanup.
هر native scenario فقط ۶۴ سند و ۳۰ عملیات دارد؛ graph شامل ۱۹۲ edge است. native build واقعی ۰.۴.۱، Release، GNU11.4، RocksDB8.5.3 و sanitizerها خاموش گزارش شد.

اجرای ابزار با سه block و نه invocation مستقل:

```bash
python3 scripts/research_ab.py run \
  --config nexora-scale-lab/config/research-smoke-v1.json \
  --directory build/research-phase0-20261028-final
python3 scripts/research_ab.py analyze --directory build/research-phase0-20261028-final
```

مبنای سورس ثبت‌شده در manifest:

- commit: `3930b5ada1b7b3a715acaeacbe3c62b11af0c8a3`
- working tree dirty است؛ فایل‌های جدید و config/source hash نیز ثبت شدند.
- source SHA در زمان همان run: `fb980af1518c74769725eaf4ad44a0708ee12af0867857ae011cc24b09a72321`
- native SHA برای هر دو arm: `73039c9c4107e9fab3fac5b5f9a621b00c13c0a1c7aacc79cb94537412acd094`
- دو arm یک binary هستند؛ **A/A control**. config نمونه مسیر اختصاصی این host را دارد.
- نتیجهٔ نهایی `descriptive-stable`، max bracket drift حدود ۶٫۹٪؛ `publication_eligible=false`.
- اجرای اولیهٔ کوتاه‌تر در `build/research-phase0-smoke-20261028` با drift حدود ۴۶٪ نامعین شد و حذف/جایگزین نشد. قالب completion آن مربوط به نسخهٔ اولیهٔ ابزار است؛ analyze نسخهٔ نهایی الزام checksum بیشتری دارد.

این run فقط وجود pipeline سالم و تشخیص noise را نشان می‌دهد؛ هیچ H1..H6 پذیرفته نشده است. اعداد ops/s این fixture برای ظرفیت موتور استفاده نشوند؛ oracle داخل timed path و RAM قرار دارد.

## ریسک‌ها و محدودیت‌ها

- سورس فعلی حین توسعه dirty است؛ source hash run قبلی با اضافه‌شدن مستندات پس از run لزوماً برابر نسخهٔ نهایی worktree نیست. manifest تاریخی تغییر نمی‌کند.
- آرشیو کامل سورس و build provenance A/B هنوز باید به pipeline بعدی اضافه شود؛ hash به تنهایی سورس را بازسازی نمی‌کند.
- driverهای رقبا برای W2، سناریوی واقعی cgroup و مقایسهٔ سه لایه هنوز آماده نیستند؛ اندازه‌گیری واقعی قابل انتشار وابسته به این‌هاست.
- آزمون امنیت H4 و workflow کاربرد H5 در این نوبت اجرا نشده‌اند.
- گزارش منابع فعلی combined engine+Python+oracle است؛ تفکیک عددی اجزای داخل همان process ادعا نمی‌شود.

## پیگیری و پذیرش فنی ۲۰۲۶-۱۰-۲۸

### تصمیم تیک‌ها

- **RES-001 تکمیل:** مشخصات versioned و مرجع صحت هر سه سناریو موجود است؛ هر سه در ۱/۱۰ worker واقعی، با barrier شروع، دو تکرار و DB تازه اجرا شدند. مقیاس بزرگ و اجرای فرضیه‌ها جزو این تیک نیست.
- **RES-002 تکمیل سند طرح:** نسخه و edition دقیق پنج محصول، مدل/guarantee/transport/index، منابع رسمی و N/A ثبت شدند. محصولات انتخاب‌شدهٔ نصب‌نشده، digest ناموجود و تفاوت PostgreSQL نصب‌شده با هدف مخفی نشده‌اند. این تیک به معنی آماده‌بودن adapters یا مجوز انتشار benchmark نیست.
- **RES-003 تکمیل تعریف/ثبت:** H1..H6 با controls، baseline و قبول/رد/نامعین ثبت و متن قراردادها همراه با SHA baseline freeze شد. هیچ فرضیه‌ای پذیرفته نشده است.
- **RES-004 باز:** زیرتسک‌های فنی آماده‌اند، اما کنترل A/A نهایی از gate نوسان عبور نکرد. baseline **artifact** ثابت/آرشیوشده است؛ اعدادش baseline عملکردی معتبر نیستند.
- **RES-005 باز:** هر دو زیرتسک فنی پاس‌اند؛ به‌علت وابستگی RES-004، تکرارپذیری breakdown عددی و پذیرش پژوهشی والد هنوز تأیید نمی‌شود.

### شواهد اصلی جدید

مسیر raw و report نهایی: `build/research-phase0-verified-20261028/`.

```bash
# از root پروژه، با Python دارای dependencies پروژه، uvicorn و httpx
python scripts/research_validate.py \
  --directory build/phase0-UNIQUE-ID --with-cgroup

# فقط ثبت طرح/فایل‌های baseline؛ benchmark تازه اجرا نمی‌کند
python scripts/research_preregister.py \
  --baseline build/research-phase0-verified-20261028/aa-control \
  --directory build/preregister-UNIQUE-ID
```

برای cgroup، root داخل WSL لازم است. در اجرای این نوبت interpreter مورد استفاده
`build/release-0.4.1-install-test/bin/python` بود. Git trust فقط با `-c safe.directory=<همین مخزن>` برای همان فرمان داده شد؛ تنظیم global تغییر نکرد.

- **۱۲ اجرای native:** سه سناریو × دو concurrency × دو تکرار؛ هر اجرا ۱۰۰۰ سند و ۲۰۰ عملیات. graph هر اجرا ۳۰۰۰ edge دارد. تمام oracleها و state نهایی پاس شدند.
- **چهار W3 واقعی:** cgroup جدید و یکتا، high=۱٫۵ GiB، max=۲ GiB، swap=0؛ عضویت child، peak، anon/file، PSI و counterهای kernel ثبت شدند. هیچ high/max/OOM/oom_kill جدیدی رخ نداد. cgroupهای فقط-آزمایشی پس از خروج child برداشته شدند؛ هیچ پردازش موجود منتقل نشد.
- **سه لایهٔ واقعی:** C++ FindMany، pybind FindMany و HTTP/1.1 persistent TCP با managed auth/ACL و NexoraQL؛ fixture/trace مشترک، ۲۰۰ پاسخ کامل در هر لایه شامل nested/null/array، و 401/403 واقعی پاس شدند.
- منابع C++ و binding به‌صورت engine+driver combined، و منابع HTTP server/client در دو tree مستقل با PID/start-time و PSS ثبت شدند؛ process مشترک یا tree همپوشان رد می‌شود. نمونه‌برداری ۲۰ms است و CPU/RSS/PSS عدد تخصیص دقیق داخلی نیست.
- A/A شامل **پنج block و ۱۵ process/DB مستقل** بود؛ هر دو arm همان native SHA را داشتند و loader دقیقاً همان فایل را تأیید کرد. snapshot سورس، خود binary و config و runtime dependencyها نگهداری شدند.
- نمونهٔ A/A نهایی max bracket drift در throughput/p99 حدود **۲۳٫۶٪** داشت؛ با threshold=۱۰٪ نتیجه `inconclusive-environment-drift` ثبت شد. threshold و دادهٔ تاریخی تغییر نکردند.
- کالیبراسیون ازپیش‌اعلام‌شده با ۲۰۰۰ عملیات در `build/research-aa-calibration-20261028/` هم نامعین بود (حدود **۴۹٫۶٪** max bracket drift). نمونهٔ کوتاهِ قبلیِ stable نیز در پروندهٔ تاریخی حفظ شده است؛ فقط بهترین run انتخاب نشده است.
- همهٔ reportها `publication_eligible=false` و `hypotheses_accepted=[]` هستند.

ثبت نهایی طرح: `build/research-preregistered-20261028/registration.json`؛
native SHA ثابت همان `73039c9c4107e9fab3fac5b5f9a621b00c13c0a1c7aacc79cb94537412acd094` است.
در registration، `baseline_measurements_stable=false` صریح است. candidate B هنوز انتخاب نشده و هیچ A/B بهبود واقعی ادعا نمی‌شود.

### تست‌ها

```bash
NEXORA_RESEARCH_NATIVE=1 NEXORA_RESEARCH_LAYERS=1 NEXORA_RESEARCH_CGROUP=1 \
  build/release-0.4.1-install-test/bin/python -m unittest discover \
  -s tests -p 'test_research*.py' -v
```

۲۲ تست (بدون skip با opt-inها و root) پاس شدند؛ شامل native واقعی، انتخاب `.so` کپی‌شدهٔ مستقل، TCP/auth/ACL، cgroup واقعی، oracle منفی، PID reuse/tree overlap، drift، CI seedدار، timeout، evidence checksum، preregistration immutable و عدم overwrite.

### ادامهٔ لازم برای RES-004/005

- [x] ثبت هم‌زمان frequency/governor (در صورت دسترسی)، CPU/IO pressure و وضعیت host در هر bracket در ابزار اضافه و کوتاه آزمایش شد؛ علت drift از این خروجی به‌تنهایی معلوم نیست و نباید بدون شاهد به Windows/WSL یا موتور نسبت داده شود.
- [x] امکان warm-up روی workerهای واقعی و اعمال CPU affinity فقط برای processهای آزمایشی در پروفایل جدید اضافه و کوتاه آزمایش شد؛ این تیک شاهد اجرای طولانی روی host آرام نیست.
- [ ] اجرای کنترل A/A جدید روی host بدون بار هم‌زمانِ کنترل‌نشده؛ threshold پس از دیدن نتیجه relax نشود؛ طرح تغییرکرده نسخهٔ تازه و همهٔ rawها را نگه دارد.
- [ ] رسیدن به کنترل قابل قبول و سپس تکرار سه‌لایه؛ CI پهن یا drift بالا اجازهٔ نسبت‌دادن تغییر به patch نمی‌دهد.
- [ ] پیش از peer ranking: نصب/تأیید runtimeهای انتخاب‌شده، effective config و digest، adapters و oracle و بررسی شرایط انتشار. این‌ها با تهیهٔ ماتریس انجام‌شده فرض نشده‌اند.

داده‌های Mongo/PostgreSQL و benchmarkهای تاریخی دست‌نخورده‌اند. هیچ تست ۱۰M/۱۰۰M، انتشار، tag یا commit انجام نشد.

## پیگیری COR-001 و پذیرش والدها — ۲۰۲۶-۱۰-۲۸

[COR-001 تکمیل شد](../cpp/PAGINATION_CONTRACT.md): ثابت‌های مشترک تعداد/بایت، خطای زودهنگام،
حذف clamp پنهان در adapter، تفکیک load batch از validation page، API بستن token و تست‌های واقعی.
هسته و extension و probe از سورس تغییرکرده rebuild شدند؛ نتیجهٔ قدیمی به binary جدید نسبت داده نشد.

شاهد کوتاه ابزار پذیرش جدید: `build/phase0-acceptance-wiring-cor001-20261028/`.
هر دو A/A و سه لایه تا completion اجرا شدند؛ seal و analyze معتبرند، ولی `parent_acceptance`
برای هر دو والد false است. short profile عمداً شرط پذیرش را برآورده نمی‌کند؛ نوسان مشاهده‌شده پنهان نشده است.
مسیر و [فرمان دقیق اجرای طولانی](PARENT_ACCEPTANCE_FA.md) آماده است؛ این اجرا توسط AI شروع نشده است.
RES-004/005 تا شاهد بلندِ پایدار باز می‌مانند. baseline و registration قبلی تغییر نکردند.

بازاجرای نهایی تست‌های کوتاه پس از تغییرات:

- native: **۶۱/۶۱ موفق**؛ pagination binding: **۵/۵ موفق**.
- research با opt-inهای native/layers/cgroup: **۲۸/۲۸ موفق**، شامل TCP و cgroup واقعی.
- lab: **۵۹ تست، ۵۲ موفق و ۷ skip اختیاری**؛ نه شاهد اجرای peerها.
- API streaming، cursor ACL، memory reporting و native threading: **۲۳/۲۳ موفق**؛
  یک هشدار deprecation مربوط به ترکیب Starlette TestClient/httpx ثبت شد.
- checker نسخه: تمام منابع نسخه با **۰٫۴٫۱** همخوان‌اند؛ نسخه عوض نشد.

برای اجرای lab از root، `PYTHONPATH=nexora-scale-lab` لازم است؛ برای subprocess تست threading،
`PYTHONPATH=/home/parham/NexoraDB` به محیط تست داده شد. اجرای اولیه بدون این مسیرها import error داشت؛
پس از اصلاح محیط اجرای تست، مجموعه‌ها پاس شدند. این‌ها رفع خطای محیط تست‌اند، نه پنهان‌کردن شکست correctness.
هیچ اجرای چندساعته، ۱۰M/۱۰۰M، حذف دادهٔ موجود یا commit در این پیگیری انجام نشد.
