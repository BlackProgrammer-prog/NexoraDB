# semantics نوع و مقایسه — قرارداد و وضعیت COR-004

## وضعیت جاری — ۲۰۲۶-۱۰-۳۰، ایندکس v3

COR-004 در محدودهٔ scalarهای جدول تاریخی پایین پذیرفته شد؛ رفتار missing/null/container همان قرارداد باقی مانده است.
بخش «آرشیو» پایین، وضعیت قبلی است و checkboxهای باز و XFAILهای آن **وضعیت جاری نیستند**.

- [x] equivalence واحد int/double/±zero: double دقیقاً صحیح در محدودهٔ int64 همان کلید integer را دارد؛ integer به double گرد نمی‌شود.
- [x] metadata با `NXI3`، کلید `idx3`، `kFormatVersion=3` و manifest/fence نسل ۳؛ نام تاریخی namespace C++ یعنی `indexv2` به معنی فرمت روی دیسک نیست.
- [x] source بسته و read-only → کپی تازه/audit → rebuild → verify → انتشار اتمیک نشانگر کامل؛ برخورد unique گزارش می‌شود و هیچ سندی حذف نمی‌شود.
- [x] تست SIGKILL وسط کپی، رد مقصد ناقص، retry تازه، reopen با reader جدید، رد مقصد توسط binary واقعی قبلی و حفظ source برای rollback.
- [x] evaluator/scan/index/unique با corpus مشترک؛ insert/update race، delete/reuse، rollback batch و unique مرکب؛ هر سه XFAIL قدیمی اکنون تست عادی موفق‌اند.

EQ عددی اکنون واقعاً `index_equality` اجرا می‌کند؛ **range عددی هنوز full scan صحیح است**، چون type-tagهای integer و float غیرصحیح ترتیب عددی سراسری نمی‌دهند.
این محدودیت performance باقی است. sparse null/container و prefix-only مرکب ناامن همچنان fallback دارند.
golden جدید: int1/double1.0 برابر `018000000000000001`؛ int0/±zero برابر `018000000000000000`؛ booltrue برابر `0301`.
مرزهای int64، ۲^۵۳، subnormal، doublemax و ۲۰ مقدار pairwise بدون گردکردن integer آزموده شده‌اند.

شواهد جدید: ۷۳ تست native موفق؛ corpus ۲۶ سند/۱۲۸ شرط؛ migration با binary واقعی قبلی هم برای source بدون manifest و هم manifest نسل ۲ اجرا شد.
۱۲ SIGKILL/reopen در شش مسیر دوام نیز روی binary نهایی موفق بود: `build/cor004-v3-final-crash-20261030/report.json`؛ این شاهد process crash است، نه قطع برق.
مجموعهٔ مرتبط Python/API/threading: **۸۵ موفق**؛ research/pagination/version و دو مجموعهٔ semantics/migration: **۴۳ موفق و ۴ skip** (با مجموعهٔ ۸۵تایی همپوشانی دارد؛ جمع نزنید).
lab: **۵۳ موفق و ۷ skip اختیاری**؛ version checker برای 0.4.1 موفق است. تست‌های migration در این میزبان skip نشده‌اند.
SHA-256 هر دو کپی native ساخته‌شده و بسته‌بندی‌شونده یکسان است: `95b26e08af1a453405441de5d3bf00edb342d8fba48386e099291a8c1fcb2c42`.

راهنمای اجرا و محدودیت‌ها: [مهاجرت ایندکس v3](INDEX_V3_MIGRATION_FA.md).
مهاجرت checkpoint مستقل اضافی نمی‌سازد: source بسته، snapshot ثابت و منبع دست‌نخورده حفظ می‌شوند.
rollback پس از write تازه روی مقصد نیازمند reconciliation است؛ graph sidecar خارجی کپی نمی‌شود.
این پذیرش معادل COR-005 isolation عمومی، numeric range سریع یا اثبات migration در مقیاس ۱۰۰M نیست.
هیچ migration روی DB کاربر، تست ۱۰M/۱۰۰M، commit، tag یا release انجام نشده است.

فرمان کوتاه داخل WSL پس از rebuild و همگام‌سازی native:

```bash
build/release/tests/cpp/nexora_doc_tests --gtest_brief=1
nexora-scale-lab/.venv/bin/python -m pytest -q tests/test_type_semantics_gaps.py tests/test_index_v3_migration.py
```

## آرشیو وضعیت قبلی — ۲۰۲۶-۱۰-۲۹ (جایگزین‌شده با v3)

تمام متن زیر ثبت تاریخی است؛ عبارت‌های «فعلی»، «باقی‌مانده» و شماره‌های تست آن به ۲۰۲۶-۱۰-۲۹ اشاره دارند.

۲۰۲۶-۱۰-۲۹. **COR-003 تکمیل است؛ COR-004 هنوز پذیرش کامل ندارد.**
اصلاح‌های این نوبت نتیجهٔ نادرست plan را متوقف می‌کنند، ولی اختلاف unique عددی نسخهٔ ۲ هنوز حل نشده است.
این سند رفتار Nexora را تعریف می‌کند؛ معادل‌بودن کامل با SQL یا MongoDB ادعا نمی‌شود.

## قرارداد مقایسهٔ scalar

| نوع/حالت | equality و ترتیب | index/unique فعلی |
| --- | --- | --- |
| int64 | محدودهٔ ‎−۲^۶۳ تا ۲^۶۳−۱؛ مقایسهٔ دقیق integer | کلید v2 از double جداست |
| float64 | فقط finite؛ مقایسه با int64 بدون گردکردن قبلی integer؛ `1 == 1.0` و `-0.0 == +0.0` | v2 این equivalence را canonical نمی‌کند؛ query عددی فعلاً scan |
| bool | فقط bool هم‌نوع برابر است؛ true با ۱ برابر نیست؛ order scalar پشتیبانی نمی‌شود | equality index مجاز؛ کلید مستقل از int/string |
| string | فقط string؛ ترتیب lexicographic بایت UTF-8؛ بدون collation یا Unicode normalization | empty string، Unicode و U+0000 قابل ایندکس‌اند |
| null | EQ فقط با null موجود؛ order ندارد | sparse: وارد index/unique نمی‌شود |
| missing | EQ/NEQ/IN/NIN و order روی leaf همگی false؛ `EXISTS false` آن را تشخیص می‌دهد | sparse: وارد index/unique نمی‌شود |
| array/object | scalar EQ با متن JSON برابر نیست؛ scalar order false؛ scalar NEQ روی مقدار موجود true | multikey/structural equality و unique کانتینری نداریم؛ sparse skip |
| مقدار نامعتبر | ثابت typed غیرقابل parse یا overflow خطای query؛ NaN/Infinity و عدد خارج از حدود داده رد می‌شوند | encode ثابت نامعتبر `nullopt` می‌دهد، نه تبدیل به string/false |

برای `IN`/`NIN` equivalence همان EQ است؛ لیست typed مختلط معتبر است.
`NOT(EQ)` به دلیل منطق boolean می‌تواند روی missing با `NEQ` فرق کند؛ منطق سه‌ارزشی SQL پیاده نشده است.
مقایسهٔ عددی روی scalar است؛ array به‌صورت خودکار پیمایش/flatten یا با عنصرش برابر نمی‌شود.
برای اعداد مرز ۲^۵۳ و ۲^۶۳ از `CompareIntDouble` استفاده می‌شود؛ integer ابتدا به double گرد نمی‌شود.
JSON integer literal خارج از int64، `1e999` و extended NaN در تست ingress رد شدند.

ثابت native Bool فقط `true`/`false`/`1`/`0`، Int64 فقط نمایش قابل parse در محدوده، و Float64 فقط مقدار finite می‌پذیرد.
empty string **فقط** با نوع String معتبر است؛ `ValueType::Null` ثابت null را مشخص می‌کند، نه رشتهٔ `"null"`.
منظور از order این جدول predicateهای GT/GTE/LT/LTE است، نه تضمین ترتیب خروجی scan یا `ORDER BY` عمومی.

## اصلاح‌های اعمال‌شده، بدون تغییر encoding

1. plan عددی v2 برای EQ/range دیگر انتخاب نمی‌شود: یک seek در یک type-tag نمی‌تواند هم int و هم double برابر را پوشش دهد.
   null نیز به دلیل sparse بودن index نباید با seek خالی، پاسخ ناقص موفق بسازد.
2. index مرکب فقط وقتی پوشش candidate امن است انتخاب می‌شود: همهٔ componentها باید با equality scalar قابل ایندکس
   یا range رشته‌ای در component آخر پوشیده باشند. prefix-only روی sparse compound می‌تواند null/missing/array فیلد بعدی را جا بیندازد.
3. codecِ query constant دیگر empty Int/Float/Bool را string خالی فرض نمی‌کند و Bool نامعتبر را false encode نمی‌کند.
4. JSON قبلاً string دارای U+0000 را می‌پذیرفت ولی decode/BSON ingress آن را invalid UTF-8 اعلام می‌کرد؛
   اکنون UTF-8 validation با اجازهٔ NUL **در string طول‌دار** در هر دو مسیر اعمال می‌شود.
   اعتبارسنجی UTF-8 خاموش نشده و NUL در field-name cstring مجاز نشده است.

این fallback ممکن است query عددی یا prefix مرکب را کندتر کند؛ نتیجهٔ صحیح بر plan سریعِ ناقص مقدم است.
قابلیت numeric index سریع حذف دائمی نشده؛ تا codec/version مناسب، استفادهٔ ناامن مسدود شده است.
تست قدیمی numeric range که بدون پوشش mixed-type، صرفاً انتخاب index را انتظار داشت به fallback صحیح تغییر کرد؛
assertهای صحت count/exists آن حفظ شدند. index equality رشته‌ای همچنان واقعی تست می‌شود.

`indexv2::kFormatVersion` هنوز **۲** و کلیدهای معتبر v2 دست‌نخورده‌اند؛ golden hex برای string/NUL/int/double/±zero/bool ثبت شد.
تغییر flags اعتبارسنجی نیز byte encoding سند را عوض نمی‌کند. هیچ rebuild، migration یا تغییر دادهٔ موجود اجرا نشد.

## شکاف باز unique و پذیرش لازم

هدف قرارداد: unique برای scalarهای عددی همان equivalence عددی EQ را رعایت کند.
نسخهٔ ۲ فعلی به دلیل type-tag/bit encoding، این سه جفت را جدا می‌پذیرد:

- int `1` و double `1.0`؛
- double `+0.0` و double `-0.0`؛
- int `0` و double `-0.0`.

این‌ها محدودیت مطلوب محصول نیستند؛ **bug/شکاف پذیرشِ ثبت‌شده‌اند**.
سه تست `xfail(strict=True)` در `tests/test_type_semantics_gaps.py` duplicate را طبق قرارداد هدف ردشده انتظار دارند
و اکنون شکست شناخته‌شده دارند. XFAIL به معنی پاس COR-004 نیست؛ پس از اصلاح، strict XPASS نیز CI را متوجه به‌روزرسانی تست می‌کند.
تست C++ unique روی تمام corpus نیز رفتار **مشاهده‌شدهٔ typed v2** را ثبت می‌کند، نه تأیید numeric equivalence مطلوب را.

## تسک‌های باقی‌مانده برای تیک والد

- [ ] **COR-004-A:** codec عددی جدید با equivalence واحد int/double/±zero و ترتیب دقیق طراحی کن؛ از تبدیل همهٔ intها به double استفاده نکن.
  corpus مرزهای ۲^۵۳، int64 min/max، subnormal، double max و compound را گسترش بده و golden نسخهٔ جدید بساز.
- [ ] **COR-004-B:** نسخهٔ index و reader fence/manifest را صریح اضافه کن؛ reader قدیمی نباید بتواند با unique قدیمی، دادهٔ ناسازگار بنویسد.
  فقط تغییر `kFormatVersion` یا normalize صفر در همان v2، migration معتبر نیست.
- [ ] **COR-004-C:** مسیر checkpoint → dry-run/audit → rebuild تازه → verify → publish اتمیک metadata را بساز/تکمیل کن؛
  uniqueهای قدیمی دارای duplicate عددی باید گزارش و متوقف شوند، نه اینکه رکوردی خودکار حذف شود.
- [ ] **COR-004-D:** crash در میانهٔ migration، rollback، reopen نسخه‌ها، concurrent insert/update/delete و unique مرکب را تست کن؛
  تا پیش از publish کامل، plan نسخهٔ تازه فعال نشود. دادهٔ قدیمی خراب/unknown version fail-closed باشد.
- [ ] **COR-004-E:** همان corpus و oracle را روی evaluator، scan، index جدید و unique اجرا کن؛
  numeric plan واقعی (نه فقط fallback) و رفع هر سه XFAIL را ثابت کن، سپس والد را تیک بزن.

## شواهد این نوبت

- `tests/cpp/test_type_semantics.cpp`: corpus شامل **۲۶ سند** و **۱۲۸ شرط** برای EQ/NEQ/range/IN/NIN؛
  scan اجباری و plan منتخب با evaluator تطبیق داده شدند. expectedهای مستقل برای mixed numbers، مرزها، null/missing و container نیز وجود دارند.
- codec روی همان corpus با typed constants و golden v2 تطبیق داده شد؛ unique v2 با دو دور corpus بررسی شد.
- C++ کامل: **۷۲ تست موفق**؛ Python مرتبط: **۶۸ موفق و ۳ شکست شناخته‌شدهٔ XFAIL**، نه ۷۱ تست پذیرفته‌شده.
- pagination: **۶ موفق**؛ lab: **۵۳ موفق و ۷ skip اختیاری**؛ نسخهٔ release همچنان ۰٫۴٫۱ همخوان است.
- COR-003 دوباره با **۶ SIGKILL/reopen** در `build/cor004-durability-regression-20261029/report.json` پاس شد؛
  scope همان process crash است، نه قطع برق. نتایج crash قبلی حفظ شدند.

فرمان‌های کوتاه از root پروژه داخل WSL پس از rebuild و همگام‌سازی binary:

```bash
build/release/tests/cpp/nexora_doc_tests
nexora-scale-lab/.venv/bin/python -m pytest -q tests/test_type_semantics_gaps.py
```

وجود XFAIL و fallback عددی در release notes/known issues باید باقی بماند تا migration تکمیل شود.
هیچ تست چندساعته/۱۰M/۱۰۰M، حذف داده، migration روی DB موجود، tag یا commit در این نوبت انجام نشده است.
