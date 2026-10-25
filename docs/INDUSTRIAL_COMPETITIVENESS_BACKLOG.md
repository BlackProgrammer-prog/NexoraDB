# بک‌لاگ رقابت صنعتی NexoraDB

تاریخ بررسی: ۲۰۲۶-۱۰-۲۱ — مبنا: شاخهٔ `develop`، commit پایهٔ `72d61cd` و فایل‌های موجود در working tree.

این سند **فهرست کار اجرایی است، نه گواهی آمادگی تولید**. بررسی شامل مسیرهای اصلی C++/RocksDB، query، graph، binding، Python/API/driver، پنل، ابزارهای نگهداری، تست و انتشار است؛ ادعای اثبات صحت تک‌تک خطوط یا کشف تمام آسیب‌پذیری‌ها ندارد. در این نوبت کد اجرایی اصلاح نشده است.

تکمیل برای عرضهٔ شرکتی: علاوه بر IND-001 تا IND-071، تسک‌های **IND-072 تا IND-091** و دروازه‌های عرضه در انتهای سند اضافه شده‌اند. «پایان توسعه» تنها وقتی به «اجازهٔ عرضه» تبدیل می‌شود که شواهد پذیرش برای **نسخه، کاربرد و محیط مشخص** تأیید شوند؛ هیچ checklist ثابتی تضمین نبودن bug یا آسیب‌پذیری آینده نیست.

## محدوده و موارد مستثنا

طبق درخواست، این‌ها دوباره به‌عنوان تسک ثبت نشده‌اند: افزودن ML، تکمیل cursor مربوط به JOIN، مجوزهای سطری/فیلدی/گراف، آزمون‌های چندروزهٔ تولید و صدور مجدد tokenهای production. وضعیت آن‌ها در [سند قبلی](cpp/CURSORS_POOL_ACL.md) مستقل باقی می‌ماند. «معنای صحیح JOIN» در این سند با «cursor انتقال JOIN» متفاوت است.

قابلیت‌های موجود مانند WAL نسخه‌بندی‌شده، compaction اتمیک، تست sanitizer، bulk builder، CSR، backup آفلاین، admission، connection pool و cursor ساده، از صفر دوباره درخواست نشده‌اند؛ تسک‌ها دربارهٔ شکاف باقی‌مانده یا اثبات قرارداد آن‌هاست. همهٔ تیک‌ها عمداً باز هستند؛ «کد وجود دارد» معادل «معیار خروج صنعتی پاس شده» نیست.

## تعریف اولویت و اندازه

| اولویت | معنی | شرط خروج |
|---|---|---|
| P0 | خطر از دست‌رفتن/ناسازگاری داده، پاسخ غلط، دسترسی ناامن یا توقف کنترل‌نشده | قبل از ادعای production برای workload مربوط حل شود |
| P1 | پیش‌نیاز نسخهٔ تک‌نودی قابل اتکا و رقابت در یک بازار مشخص | قبل از پایلوت مشتریِ همان قابلیت و ارائهٔ SLA |
| P2 | قابلیت عملیاتی/سازگاری و پوشش کاربرد گسترده‌تر | پس از پایهٔ P0/P1، بر اساس مشتری هدف |
| P3 | گسترش محصول و معماری چندنودی با وابستگی زیاد | پس از تثبیت تک‌نودی؛ اگر HA وعده داده شود، تسک‌های HA به شرط انتشار ارتقا می‌یابند |

اندازه‌ها تخمین نسبی‌اند: **S** محلی، **M** چند ماژول، **L** زیرسیستم، **XL** معماری/چند مرحله. این‌ها زمان‌بندی یا تعهد تحویل نیستند.

برچسب شواهد: **مشاهده** یعنی رفتار/محدودیت در کد یا قرارداد فعلی دیده شده؛ **ریسک** یعنی نیازمند تست برای تأیید یا رد؛ **شکاف** یعنی پیاده‌سازی یا شاهد کافی در مسیرهای بررسی‌شده پیدا نشد. «شکاف» ادعای نبودن قطعی در هر فایل مخزن نیست.

## شواهد و نقشهٔ فایل‌ها

| کد | مشاهدهٔ مبنا و محل پیگیری |
|---|---|
| E01 | [semantic.py](../NexoraDB/src/nexoradb/parser/semantic.py): callback گراف بعد از insert/update/delete؛ مسیر native مستقل است؛ bulk update/delete همان اطلاع‌رسانی point را ندارند. سند ورودی INSERT می‌تواند با سند واقعی دارای defaults/ID تولیدشده متفاوت باشد. |
| E02 | [GraphManager.cpp](../graph/GraphManager.cpp): `buildNodeFromDoc` بدنهٔ filter را اجرا نمی‌کند؛ properties در serialization تعریف دیده می‌شود؛ `onDocumentUpdated` node را remove/re-add می‌کند؛ حذف edgeهای UNWIND پوشش کامل ندارد. |
| E03 | همان فایل: `buildGraph` وضعیت Building می‌نویسد و گراف فعال را clear می‌کند؛ scanهای collectionها جدا هستند. این با generation-safe compaction موجود یکی نیست. |
| E04 | [DBMS_HARDENING.md](cpp/DBMS_HARDENING.md) و [PRODUCTION_RECOVERY.md](cpp/PRODUCTION_RECOVERY.md): outbox اتمیک سند↔گراف و تشخیص عقب‌ماندگی source قرارداد کامل فعلی نیست؛ backup، وضعیت quiescent موجود را نگه می‌دارد. |
| E05 | [DocEngine.cpp](../core/DocEngine.cpp)، [DocEngine.h](../core/DocEngine.h): TransactionDB و تراکنش وجود دارد؛ قرارداد isolation، conflict، phantom و FK باید در سطح محصول اثبات شود. FK در مسیر point از Get استفاده می‌کند؛ این به‌تنهایی اثبات نقض FK نیست. |
| E06 | [RocksTuning.h](../core/RocksTuning.h): پیش‌فرض `DocumentDurability::Buffered`؛ [native.py](../NexoraDB/src/nexoradb_admin/native.py) موتور را با تنظیمات پیش‌فرض می‌سازد. |
| E07 | [idempotency.py](../NexoraDB/src/nexoradb_admin/idempotency.py) و [API](../NexoraDB/src/nexoradb/api/app.py): journal مستقل SQLite، reservation پایدار، وضعیت نامعلوم fail-closed، سقف ظرفیت و barrier جدا؛ تراکنش مشترک RocksDB نیست. |
| E08 | [CompiledQuery.cpp](../query/CompiledQuery.cpp): `std::regex`/`regex_search`؛ محدودیت AST/input، تضمین زمان اجرای regex نیست. |
| E09 | [DocEngine.cpp](../core/DocEngine.cpp): انتخاب index با score ثابت equality/range؛ [Condition.h](../query/Condition.h): قرارداد QueryOptions و sorting نیازمند تطبیق با اجرای واقعی. |
| E10 | [IndexCodec.cpp](../core/IndexCodec.cpp)، [NumericCompare.h](../query/NumericCompare.h)، [NEXORAQL_CONTRACT.md](cpp/NEXORAQL_CONTRACT.md): کدگذاری typed وجود دارد؛ mixed numeric، unique، null/missing و JOIN نیازمند قرارداد یکسان‌اند. |
| E11 | [query_runner.py](../NexoraDB/src/nexoradb_admin/query_runner.py)، [semantic.py](../NexoraDB/src/nexoradb/parser/semantic.py): normalization و projection در Python، پاسخ rows/raw، materialization در برخی مسیرها؛ JOIN فعلی first-match lookup است، نه JOIN رابطه‌ای کامل. |
| E12 | [admin app](../NexoraDB/src/nexoradb_admin/app.py)، [security](../NexoraDB/src/nexoradb_admin/security.py)، [service](../NexoraDB/src/nexoradb_admin/service.py): امضای token کنترل می‌شود؛ current-token dependency وضعیت زندهٔ کاربر/role را در همهٔ routeها بازخوانی نمی‌کند؛ Argon2 حدود 64MiB به‌ازای hash تنظیم شده؛ bootstrap و throttling باید بررسی شوند. |
| E13 | همان admin app: `/health` پاسخ ثابت دارد؛ health واقعی موتور و API دیگر جدا وجود دارد. [Operations.cpp](../core/Operations.cpp) metrics/health موجود را پیاده می‌کند. |
| E14 | [authTokenStorage.ts](../frontend/src/features/auth/services/authTokenStorage.ts): token ادمین در localStorage؛ [appToken.types.ts](../frontend/src/features/appTokens/types/appToken.types.ts): فرم/نوع صدور token هنوز collections جدید را مدل نمی‌کند. |
| E15 | [nexora_admin.py](../scripts/nexora_admin.py): checkpoint، SHA-256، staging، restore و journal backup وجود دارد؛ این‌ها به‌تنهایی PITR، backup آنلاین یا امضای معتبر artifact نیستند. |
| E16 | [native-quality](../.github/workflows/native-quality.yml)، [performance](../.github/workflows/performance.yml)، [publisher](../.github/workflows/publish-pypi.yml): CI موجود است؛ publisher در bootstrap مثل job کیفیت checkout ثابت vcpkg ندارد؛ actionها عمدتاً tag-based هستند. |
| E17 | [pyproject.toml](../NexoraDB/pyproject.toml)، [CMake](../CMakeLists.txt)، [vcpkg](../vcpkg.json): wheel فعلی Linux x86_64/CPython 3.10؛ RocksDB بررسی‌شده 8.5.3؛ پشتیبانی پلتفرم و نگهداری dependency مستقل از correctness است. |
| E18 | [README](../README.md) MIT را اعلام می‌کند، ولی [LICENSE](../LICENSE) و pyproject، Apache-2.0 هستند؛ README دربارهٔ live graph/ACID نیز باید با محدودیت‌های واقعی همخوان شود. |
| E19 | [GraphManager](../graph/GraphManager.cpp)، [algorithms](../graph/algorithms)، [memory](cpp/GRAPH_MEMORY.md): snapshot/dense/budget وجود دارد؛ semantics و صحت الگوریتم‌ها و بودجهٔ مشترک کل process باید جدا اثبات شود. |
| E20 | در ماژول‌های core/graph/transport بررسی‌شده، مسیر کامل consensus/replication/fencing/PITR پیدا نشد؛ نباید WAL محلی RocksDB/Graph را معادل replication دانست. |

## P0 — اعتماد به داده و مرز امنیت

- [ ] **IND-001 · P0 · XL — outbox/change log اتمیک در منبع سند**
  شواهد: مشاهده E01/E04. تغییر document، index، counter و event در همان transaction ذخیره شود؛ تمام مسیرهای native، Python، HTTP، bulk و explicit transaction پوشش داشته باشند.
  معیار پذیرش: kill قبل/بعد از commit و قبل از callback هیچ commit بدون event نسازد؛ replay event روی گراف idempotent باشد. وابستگی پایهٔ IND-002 و IND-065.

- [ ] **IND-002 · P0 · L — پروتکل تطبیق source و projection هنگام startup**
  شواهد: شکاف E04. database UUID، source sequence، mapping version و applied sequence ذخیره و مقایسه شوند؛ gap، rollback منبع و backup قدیمی قابل تشخیص باشند.
  معیار پذیرش: گراف سالمِ عقب‌مانده یا متعلق به DB دیگر healthy نشود؛ فقط replay بازهٔ کامل یا rebuild معتبر، readiness را باز کند. وابسته به IND-001.

- [ ] **IND-003 · P0 · L — rebuild گراف در generation تازه، بدون تخریب نسخهٔ فعال**
  شواهد: مشاهده E03. مسیر build را با compaction امنِ موجود اشتباه نگیریم؛ publication و فایل قبلی تا commit حفظ شوند.
  معیار پذیرش: failure در هر مرز scan/write/sync/publish، old یا new state کامل بدهد؛ old graph هنگام rebuild ناموفق قابل بازیابی باشد.

- [ ] **IND-004 · P0 · M — رویداد بر اساس post-image واقعی، نه JSON ورودی**
  شواهد: مشاهده E01. `_id` تولیدشده، defaultها، schema normalization و revision واقعی باید وارد mapping شوند؛ bypassهای native/bulk بسته شوند.
  معیار پذیرش: INSERT بدون ID و دارای defaults در همهٔ APIها یک هویت سند و گراف تولید کند؛ native مستقیم نیز همان قرارداد داشته باشد. وابسته به IND-001.

- [ ] **IND-005 · P0 · M — اصلاح semantics به‌روزرسانی node و حفظ edgeهای نامرتبط**
  شواهد: ریسک E02؛ remove/re-add برای update دیده می‌شود. تغییر property یا کلید باید از حذف هویت تفکیک شود.
  معیار پذیرش: property-only update هیچ رابطه‌ای را از بین نبرد؛ تغییر key، هم‌پوشانی mappingها و implicit node تست شوند؛ اگر پشتیبانی نیست، خطای صریح قبل از mutation.

- [ ] **IND-006 · P0 · L — مالکیت رابطه بر اساس source document و diff کامل UNWIND**
  شواهد: مشاهده E02. update/delete آرایه باید رابطه‌های قدیمی را حذف و جدید را اضافه کند؛ dedup دو سند نباید حذف یکی را به حذف رابطهٔ دیگری تبدیل کند.
  معیار پذیرش: shrink/reorder/delete آرایه، retry و restart نتیجه‌ای برابر rebuild مرجع داشته باشند؛ reference counting یا edge identity مستند شود.

- [ ] **IND-007 · P0 · M — filter/property mapping اجرا شود یا از ابتدا رد شود**
  شواهد: مشاهده E02. پذیرفتن تعریف و نادیده‌گرفتن آن قابل قبول نیست؛ full build و incremental update از evaluator مشترک استفاده کنند.
  معیار پذیرش: سند خارج از filter وارد گراف نشود؛ propertyها مطابق نوع/مسیر منتقل شوند؛ unsupported mapping قبل از persist رد شود.

- [ ] **IND-008 · P0 · M — انتخاب صریح durability در سطح سرویس و هر acknowledgment**
  شواهد: مشاهده E06. production نباید ناخواسته از Buffered استفاده کند؛ durable/group/async و هزینه/تضمینشان در config و پاسخ قابل تشخیص باشند.
  معیار پذیرش: تست deployment نشان دهد سرویس واقعاً profile انتخاب‌شده را به C++ می‌رساند؛ commit تأییدشده با تضمین اعلام‌شده در crash بازیابی شود، نه صرفاً با flush پایتون.

- [ ] **IND-009 · P0 · L — receipt تراکنشی درخواست‌ها و ابزار reconciliation**
  شواهد: مشاهده E07. اثر document و receipt در یک commit domain ذخیره شوند؛ اثر گراف از outbox عبور کند؛ unknown outcome مسیر بررسی عملیاتی داشته باشد.
  معیار پذیرش: crash بین اثر و پاسخ باعث دوباره‌نویسی نشود؛ key فعال/نامعلوم evict نشود؛ restore داده و receipt هم‌نسخه باشد؛ full/permission error تست شود. این کار بازنویسی pooling نیست.

- [ ] **IND-010 · P0 · L — قرارداد isolation تراکنش و اثبات anomalyها**
  شواهد: ریسک E05. سطح isolation واقعی، read-your-writes، lost update، write skew، phantom و commit conflict مشخص شوند؛ unsupported scope قبل از اجرا رد شود.
  معیار پذیرش: history checker و تست interleaving چند connection مطابق سطح اعلام‌شده پاس شوند؛ صرف وجود TransactionDB معیار قبولی نیست.

- [ ] **IND-011 · P0 · M — تضمین FK زیر concurrency و policy حذف/تغییر parent**
  شواهد: ریسک E05؛ نیازمند reproduction، نه ادعای قطعی bug. read مرجع باید با lock/snapshot/constraint protocol مناسب هماهنگ باشد.
  معیار پذیرش: insert child هم‌زمان با delete/update parent، dangling FK نسازد؛ RESTRICT/CASCADE/عدم پشتیبانی صریح و atomic باشد؛ supporting index DDL نیز تست شود.

- [ ] **IND-012 · P0 · M — semantics یکسان مقایسه، index و unique**
  شواهد: ریسک E10. سیاست `1`/`1.0`، `-0.0`/`0.0`، null/missing، رشته/عدد و حدود int64 به‌صورت واحد تعریف شود.
  معیار پذیرش: scan و index برای corpus مرزی نتیجهٔ برابر بدهند؛ unique constraint مطابق equality اعلام‌شده باشد؛ هر تغییر encoding migration داشته باشد.

- [ ] **IND-013 · P0 · L — cancellation و deadline در خود اجرای native**
  شواهد: مشاهده E08/E11. timeout HTTP یا رهاکردن thread، عملیات C++ را متوقف نمی‌کند؛ scan/update/index build باید safe point داشته باشند.
  معیار پذیرش: درخواست لغوشده در بازهٔ مستند CPU/lock/snapshot را آزاد کند؛ write نیمه‌کاره commit ناخواسته نداشته باشد؛ thread kill ناامن استفاده نشود.

- [ ] **IND-014 · P0 · M — حذف ریسک ReDoS از regex قابل ارسال توسط کاربر**
  شواهد: مشاهده E08. موتور با تضمین زمانی مناسب یا subset امن و بودجهٔ واقعی انتخاب شود؛ محدودیت طول به‌تنهایی کافی نیست.
  معیار پذیرش: الگوهای adversarial روی رشته‌های طولانی در بودجهٔ CPU/memory تمام شوند؛ syntax/LIKE contract، Unicode و exceptionها تست و version شوند.

- [ ] **IND-015 · P0 · M — اعتبار زندهٔ session ادمین و bootstrap امن**
  شواهد: مشاهده/ریسک E12. تغییر status/role/password یا logout امنیتی باید session را باطل کند؛ نصب اولیه به first visitor عمومی واگذار نشود.
  معیار پذیرش: کاربر disableشده با token قدیمی به routeهای admin و Socket.IO دسترسی نداشته باشد؛ bootstrap یک‌بارمصرف و محدود به مسیر امن باشد.

- [ ] **IND-016 · P0 · M — بودجهٔ احراز هویت و throttling مستقل**
  شواهد: مشاهده E12؛ Argon2 حدود 64MiB برای هر hash دارد. concurrency کلی ۱۶، بودجهٔ امن برای ۱۶ hash هم‌زمان را تضمین نمی‌کند.
  معیار پذیرش: login/register اشتباهِ هم‌زمان OOM یا starvation query نسازد؛ rate limit، سقف hashهای فعال و پاسخ خطای بدون user enumeration تست شوند؛ hash ضعیف نشود.

- [ ] **IND-017 · P0 · S — تفکیک liveness، readiness و degraded state در همهٔ ورودی‌ها**
  شواهد: مشاهده E13. `/health` ثابت ادمین نباید به‌عنوان سلامت داده یا projection استفاده شود.
  معیار پذیرش: corruption، background error، shutdown و projection lag بحرانی readiness را ببندند؛ liveness صرفاً زنده‌بودن process را اعلام کند؛ probe باعث mutation پنهان نشود.

- [ ] **IND-018 · P0 · M — قرارداد خطا و نتیجهٔ commit در تمام APIها**
  شواهد: مشاهده E07/E11. خطای serialization/413/شبکه پس از mutation، الزاماً rollback نیست؛ HTTP 200 نیز الزاماً موفقیت تمام statementها نیست.
  معیار پذیرش: success/partial/rolled-back/unknown به‌صورت machine-readable تفکیک شوند؛ client و پنل هیچ‌کدام write نامعلوم را خودکار با key جدید تکرار نکنند؛ تمام mutation routeها ممیزی شوند.

## P1 — تک‌نودی قابل اتکا و مزیت عملکردی قابل دفاع

- [ ] **IND-019 · P1 · L — catalog پایدار با ID، dependency و generation**
  شواهد: شکاف E04/E05. نام فایل/collection به‌تنهایی شناسهٔ پایدار database/index/graph نیست.
  معیار پذیرش: rename/drop/recreate شیء هم‌نام، metadata و plan قدیمی را به شیء جدید وصل نکند؛ schema/index/FK/graph dependency در catalog versioned باشد.

- [ ] **IND-020 · P1 · L — schema evolution همراه validation دادهٔ قبلی**
  شواهد: مشاهده E05/E10. تغییر schema و strict/default/type باید روی داده و عملیات concurrent قرارداد روشن داشته باشد.
  معیار پذیرش: dry-run شمارش نقض‌ها بدهد؛ migration یا reject صریح انجام شود؛ schema به‌ظاهر معتبر روی دادهٔ ناسازگار publish نشود؛ object/array semantics مستند باشد.

- [ ] **IND-021 · P1 · L — online DDL و index lifecycle قابل توقف/ادامه**
  شواهد: index build موجود است؛ شکاف، اثبات کل lifecycle در E05/E10 است.
  معیار پذیرش: build/cancel/restart/drop هم‌زمان با mutation و crash، index ناقص را Ready نکند؛ disk budget و progress وجود داشته باشد؛ unique build race تست شود.

- [ ] **IND-022 · P1 · L — planner مبتنی بر آمار و هزینه**
  شواهد: مشاهده E09؛ score ثابت equality/range. آمار cardinality، skew و selectivity با invalidation/version کنترل شود.
  معیار پذیرش: در دادهٔ skewed و indexهای رقیب، chosen plan با هزینهٔ واقعی مقایسه شود؛ stale statistics مسیر fallback داشته باشد؛ plan cache مصرف حافظهٔ محدود داشته باشد.

- [ ] **IND-023 · P1 · M — Explain/Analyze قابل اتکای operator-level**
  شواهد: Explain و metrics موجودند، اما شاهد کامل هزینه/ردیف هر operator کافی نیست.
  معیار پذیرش: estimated/actual rows، scan bytes، spill، lock wait و زمان مشخص باشد؛ Explain بدون اجرای ناخواستهٔ write و بدون افشای مقادیر محرمانه کار کند.

- [ ] **IND-024 · P1 · M — مشخصات رسمی زبان و type semantics**
  شواهد: E10/E11 و NEXORAQL_CONTRACT. SQL-like بودن، سازگاری کامل SQL نیست؛ null/boolean/array/LIKE و errorهای type باید یک مرجع داشته باشند.
  معیار پذیرش: compatibility matrix و corpus اجرایی منتشر شود؛ parser، evaluator، update و index از یک قرارداد پیروی کنند؛ unsupported syntax بی‌صدا تفسیر دیگری نگیرد.

- [ ] **IND-025 · P1 · L — ORDER BY پایدار و top-k در موتور**
  شواهد: مشاهده E09. sort تعریف‌شده در API باید به execution واقعی متصل شود؛ tie-break و collation تعیین شوند.
  معیار پذیرش: LIMIT همراه ORDER BY نتیجهٔ deterministic بدهد؛ index-order یا bounded top-k استفاده شود؛ fallback بزرگ به spill محدود برود، نه sort نامحدود Python.

- [ ] **IND-026 · P1 · L — JOIN با cardinality و isolation صحیح**
  شواهد: مشاهده E11؛ first-match lookup فعلی JOIN رابطه‌ای کامل نیست. این تسک دربارهٔ semantics است، نه cursor JOIN مستثناشده.
  معیار پذیرش: one-to-many، duplicate key، no-match، target predicate و snapshot مشترک قرارداد داشته باشند؛ خروجی با oracle رابطه‌ای برای subset پشتیبانی‌شده برابر باشد.

- [ ] **IND-027 · P1 · L — aggregation و expression engine نوع‌دار**
  شواهد: شکاف پوشش SQL-like. GROUP BY/HAVING و SUM/AVG/MIN/MAX فقط در صورت نیاز بازار هدف اضافه شوند.
  معیار پذیرش: overflow/decimal/null و aggregation خالی مشخص باشد؛ aggregation بزرگ bounded/spillable و دارای تست مرجع باشد؛ تابع unsupported صریح رد شود.

- [ ] **IND-028 · P1 · L — projection/predicate pushdown و حذف تبدیل‌های تکراری**
  شواهد: مشاهده E11؛ BSON→JSON→Python→JSON و projection پایتونی. بهینه‌سازی با profile، نه صرفاً تغییر نام فرمت.
  معیار پذیرش: bytes decoded/copied، CPU و allocation قبل/بعد ثبت شوند؛ فقط ستون/فیلد لازم decode شود؛ raw/rows تکراری opt-in بماند؛ ownership و GIL lifetime تست شوند.

- [ ] **IND-029 · P1 · L — اجرای operatorها با batch و حافظهٔ bounded**
  شواهد: E11؛ SELECT buffered می‌تواند قبل از کنترل پاسخ materialize شود. این کار، تضمین اجرایی هسته است نه تکرار تسک cursor.
  معیار پذیرش: scan/filter/project/aggregate interface تدریجی مشترک داشته باشند؛ اجرای limitدار کار اضافی نامحدود نکند؛ native و HTTP از یک semantics استفاده کنند.

- [ ] **IND-030 · P1 · L — memory/disk governor سراسری و spill امن**
  شواهد: E06/E19؛ budget مؤلفه‌ها hard RSS مشترک نیست.
  معیار پذیرش: cache، memtable، graph، snapshot، query، auth و temporary data زیر reservation مشترک گزارش شوند؛ workload concurrent با رد کنترل‌شده/OOM-safe تمام شود؛ spill با quota، cleanup و permission امن باشد.

- [ ] **IND-031 · P1 · M — fairness و workload management چند مشتری**
  شواهد: admission و scheduler موجودند؛ cap کلی، تضمین fairness نیست. این تسک مجوز جدید سطری/گراف نیست.
  معیار پذیرش: query سنگین یک app، مشتری دیگر یا maintenance ضروری را گرسنه نکند؛ محدودیت CPU/I/O/queued work و wait metrics قابل تنظیم باشد؛ load shedding قابل پیش‌بینی بماند.

- [ ] **IND-032 · P1 · M — سقف منابع native session/snapshot در تمام bindingها**
  شواهد: E05/E19؛ فقط HTTP cursor را محدودکردن کافی نیست.
  معیار پذیرش: caller مستقیم C++/Python نتواند snapshotهای رهاشدهٔ نامحدود نگه دارد؛ TTL/close/destructor و shutdown امن باشند؛ pinned bytes و native session count قابل مشاهده باشد.

- [ ] **IND-033 · P1 · L — snapshot مشترک منبع برای full graph build**
  شواهد: مشاهده E03؛ scanهای collections جدا انجام می‌شوند.
  معیار پذیرش: build در حضور writer یک cut سازگار از node/edge collections بسازد و revision آن ثبت شود؛ catch-up با IND-001 هماهنگ باشد، نه ترکیب چند لحظهٔ متفاوت.

- [ ] **IND-034 · P1 · M — قرارداد هویت و cardinality گراف**
  شواهد: E02/E19. نوع ExtId، multi-edge، dedup، self-loop، implicit node و property ownership باید صریح باشند.
  معیار پذیرش: insert تکی/bulk/replay/rebuild/compaction یک نتیجه بدهند؛ شناسه‌های typed و mappingهای هم‌پوشان collision خاموش نداشته باشند؛ migration semantics وجود داشته باشد.

- [ ] **IND-035 · P1 · M — oracle مستقل برای الگوریتم‌های گراف**
  شواهد: E19؛ وجود BFS/centrality/community کد، صحت همهٔ حالت‌ها را ثابت نمی‌کند.
  معیار پذیرش: directed/undirected، isolated/self-loop/multi-edge، disconnected، حذف node و remap با مرجع مستقل مقایسه شوند؛ weighted/unweighted و approximation دقیقاً اعلام شوند؛ randomness seedپذیر باشد.

- [ ] **IND-036 · P1 · M — چرخهٔ عمر امن app secret و نمایش یک‌بارهٔ token**
  شواهد: E12؛ tokenهای ذخیره‌شده decrypt و در list response برگردانده می‌شوند.
  معیار پذیرش: listing عادی secret کامل ندهد؛ rotation/revoke، last-used و expiration قابل مشاهده باشد؛ audit و backup هم token plaintext منتشر نکنند؛ migration برای رفتار قدیمی تعریف شود.

- [ ] **IND-037 · P1 · M — key rotation با جداسازی signing و encryption**
  شواهد: E12 و service token cipher؛ وابستگی کلیدها باید version شود.
  معیار پذیرش: key ID/keyring، overlap کوتاه، revoke اضطراری و re-encryption امن وجود داشته باشد؛ چرخش signing، دادهٔ رمز‌شده را غیرقابل بازیابی نکند؛ secret در repo/log نباشد.

- [ ] **IND-038 · P1 · L — audit log امنیتی مستقل از slow-query log**
  شواهد: شکاف E12/E13؛ metrics و slow ring، audit قابل اتکا نیستند.
  معیار پذیرش: auth، تغییر metadata، تنظیم امنیت، عملیات مخرب و maintenance با actor/result/correlation ثبت شوند؛ redaction، rotation، retention و tamper evidence تست شوند؛ هزینه محدود و policy پرشدن دیسک روشن باشد.

- [ ] **IND-039 · P1 · L — حفاظت داده و backup در حالت at-rest**
  شواهد: E15/E17؛ checksum فساد را تشخیص می‌دهد، محرمانگی نمی‌دهد.
  معیار پذیرش: threat model و راهکار پشتیبانی‌شدهٔ encryption/KMS برای داده، WAL، spill و backup مستند و آزمایش شود؛ restore/key loss/rotation پوشش داشته باشد؛ از ادعای محافظت در برابر root دارای کلید پرهیز شود.

- [ ] **IND-040 · P1 · M — سخت‌سازی session پنل و مرورگر**
  شواهد: مشاهده E14؛ bearer ادمین در localStorage. راهکار HttpOnly session یا token کم‌عمر همراه trade-offهای CSRF/XSS انتخاب شود.
  معیار پذیرش: CSP مناسب، origin/CSRF، logout/revoke و Socket.IO auth همخوان باشند؛ secret در خطای frontend، clipboard یا telemetry نشت نکند؛ تغییر فقط تعویض محل storage نباشد.

- [ ] **IND-041 · P1 · M — همگامی کامل پنل با قرارداد واقعی backend**
  شواهد: مشاهده E14؛ فرم token، collections جدید را مدل نمی‌کند. UI نباید قابلیت تولیدیِ کارنکرده را آماده نشان دهد.
  معیار پذیرش: schema/type تولیدشده از OpenAPI یا contract test شامل resource grants، statement error، unknown outcome، progress و cancel باشد؛ destructive action تأیید و خطای قابل فهم داشته باشد. صدور مجدد tokenهای موجود خارج این تسک است.

- [ ] **IND-042 · P1 · M — observability عملیاتی قابل اتصال به SRE**
  شواهد: E13/E19؛ metrics موجود را تکمیل و یکپارچه کنیم، نه از نو بسازیم.
  معیار پذیرش: نرخ/latency/errors، lock wait، projection lag، scheduler، WAL/compaction، disk و memory با cardinality محدود صادر شوند؛ tracing و correlation بدون query literal/PII ممکن باشد؛ alert/runbook قابل اجرا باشد.

- [ ] **IND-043 · P1 · M — lifecycle سرویس و config قابل اعتبارسنجی**
  شواهد: E06/E12/E17. start/drain/checkpoint/close، signal و ownership shared engine باید یک قرارداد داشته باشند.
  معیار پذیرش: SIGTERM تحت بار، deadline shutdown و restart بدون فساد تست شوند؛ invalid config قبل از بازکردن داده رد شود؛ filesystem permission، umask، disk reserve و effective settings قابل مشاهده باشند.

- [ ] **IND-044 · P1 · M — زنجیرهٔ تأمین و build بازتولیدپذیر**
  شواهد: E16/E17؛ نسخهٔ قدیمی dependency، خودبه‌خود اثبات CVE نیست و باید بررسی شود.
  معیار پذیرش: dependency/CVE inventory و SLA وصله، vcpkg/bootstrap یکسان و pinشده، action SHA، SBOM، provenance و artifact signing برقرار باشند؛ wheel و backend dependencyهای پشتیبانی‌شده مشخص باشند؛ sanitizerهای dependency حفظ شوند.

- [ ] **IND-045 · P1 · L — تست property/differential/metamorphic برای semantics**
  شواهد: E05/E10/E19؛ fuzz parser موجود است، ولی invariant داده فراتر از parser است.
  معیار پذیرش: scan=index، bulk=single، replay=original، rebuild=incremental، encoding roundtrip و transaction history با seed بازتولیدپذیر بررسی شوند؛ هر failure به corpus دائمی تبدیل شود.

- [ ] **IND-046 · P1 · M — gate انتشار با integration واقعی و failure matrix**
  شواهد: E16؛ تست fake API به‌تنهایی native correctness را ثابت نمی‌کند.
  معیار پذیرش: wheel نصب‌شده→driver→TCP→API→C++→RocksDB در CI تست شود؛ corruption/ENOSPC/EACCES/OOM/cancel/clock behavior پوشش مشخص داشته باشد؛ required checks واقعاً در GitHub اعمال شوند، نه صرف وجود YAML.

- [ ] **IND-047 · P1 · L — مقایسهٔ رقابتی منصفانه و قابل تکرار**
  شواهد: benchmarkهای WSL شاهد parity با رقبا نیستند. این تسک آزمون چندروزهٔ مستثناشده نیست.
  معیار پذیرش: روی سخت‌افزار ثابت، نسخه/config/durability/index/driver برابر و dataset بزرگ‌تر از RAM برای subset مشترک اندازه‌گیری شود؛ P50/P95/P99، throughput، RSS، amplification، latency زیر write/compaction و هزینهٔ هر عملیات گزارش شوند؛ ادعای رسمی YCSB/LDBC/TPC فقط با اجرای منطبق و رعایت قواعد همان benchmark باشد.

## P2 — عملیات حرفه‌ای، سازگاری و اکوسیستم

- [ ] **IND-048 · P2 · XL — backup آنلاین و point-in-time recovery**
  شواهد: مشاهده E15؛ checkpoint آفلاین موجود، PITR نیست.
  معیار پذیرش: base backup + archived log + metadata به زمان/sequence انتخابی restore شود؛ gap و log متعلق به DB دیگر reject شود؛ projection از outbox همگام شود؛ معیار RPO/RTO قرارداد باشد. وابسته به IND-001/002/019.

- [ ] **IND-049 · P2 · L — migration قابل برگشت عملیاتی و بدون نوشتن غافلگیرکننده**
  شواهد: E15/E17؛ startup می‌تواند migration آغاز کند.
  معیار پذیرش: inspect/read-only واقعاً بدون mutation باشد؛ upgrade نیازمند تأیید و backup مناسب، progress/resume/cancel و تخمین فضای دیسک باشد؛ downgrade فقط در حالت پشتیبانی‌شده یا restore صریح انجام شود.

- [ ] **IND-050 · P2 · M — مدیریت چرخهٔ عمر backup و نسخهٔ immutable خارج میزبان**
  شواهد: E15؛ hash manifest با امضای هویت backup یکی نیست.
  معیار پذیرش: retention، سهمیه، encryption، امضای manifest، off-host/immutable copy و credential کم‌اختیار وجود داشته باشد؛ حذف backup بدون اجازهٔ روشن و اعتبارسنجی نسخهٔ جایگزین انجام نشود.

- [ ] **IND-051 · P2 · L — ابزار repair/salvage کنترل‌شده و گزارش خسارت**
  شواهد: integrity/rebuild موجود است؛ تعمیر با حفظ شواهد باید جدا تعریف شود.
  معیار پذیرش: ابزار روی copy کار کند؛ object/index/segment آسیب‌دیده و رکورد ازدست‌رفته را گزارش کند؛ هیچ corrupted record را به‌عنوان success خاموش skip نکند؛ chain-of-custody برای incident حفظ شود.

- [ ] **IND-052 · P2 · L — change stream و CDC با resume و lag contract**
  شواهد: شکاف E20 و نیاز IND-001.
  معیار پذیرش: consumer بتواند با token versioned ادامه دهد؛ retention gap، delete/tombstone و schema change مشخص باشند؛ slow consumer حافظه/دیسک را نامحدود نگه ندارد؛ exactly-once بی‌پشتوانه وعده داده نشود.

- [ ] **IND-053 · P2 · L — نوع‌های دادهٔ موردنیاز بازار هدف**
  شواهد: E10/E17؛ نوع‌های پایه موجودند؛ نیازهای صنعتی باید با محصول تطبیق داده شوند.
  معیار پذیرش: برای decimal/زمان/UUID/binary مورد انتخاب، precision، timezone، ordering، codec، index، API و migration یک قرارداد مشترک داشته باشند؛ پول با float بی‌توضیح مدل نشود.

- [ ] **IND-054 · P2 · L — indexهای کاربردی فراتر از scalar پایه**
  شواهد: E10؛ اول با queryهای مشتری توجیه شود، نه تقلید همهٔ indexهای رقبا.
  معیار پذیرش: برای multikey/partial/covering/TTL انتخاب‌شده، mutation و recovery و unique semantics تست شوند؛ TTL تحت budget و با event منبع اجرا شود؛ هر index write amplification قابل اندازه‌گیری داشته باشد.

- [ ] **IND-055 · P2 · M — schema/constraintهای پیچیدهٔ لازم برای مشتری**
  شواهد: E05/E10؛ strict فعلی عمدتاً top-level است.
  معیار پذیرش: nested schema، nullable، check constraint یا validation انتخاب‌شده هم در insert و update و bulk و transaction اعمال شود؛ تغییر schema دادهٔ قدیمی را با policy مشخص مدیریت کند.

- [ ] **IND-056 · P2 · L — driver غیرمسدودکننده و session contract**
  شواهد: client pooled فعلی sync است؛ نباید مستقیم در event loop مصرف شود. ایجاد pool پایه دوباره درخواست نشده است.
  معیار پذیرش: async API واقعی با lifecycle، cancellation، محدودیت queue و error taxonomy مشابه ارائه شود؛ resourceها میان event loopها/fork اشتباه share نشوند؛ sync/async نتیجه و retry contract برابر داشته باشند.

- [ ] **IND-057 · P2 · L — driverهای زبان‌های مشتری و نسخه‌بندی protocol**
  شواهد: شکاف اکوسیستم E17. Java/.NET/Go/JavaScript طبق مشتری هدف انتخاب شوند؛ تقلید نام protocol رقبا کافی نیست.
  معیار پذیرش: compatibility/feature negotiation، typed values، timeout، TLS، retry و conformance مشترک در چند زبان تست شود؛ تغییر breaking بدون major protocol انجام نشود.

- [ ] **IND-058 · P2 · L — ابزار انتقال داده از رقبا و export قابل اتکا**
  شواهد: شکاف مسیر مهاجرت مشتری؛ dump JSON به‌تنهایی انتقال constraint/type نیست.
  معیار پذیرش: import از subset مشخص PostgreSQL/MongoDB/graph با گزارش type loss، ID، null و relationship mapping انجام شود؛ resume/checksum/dedup و quarantine رکورد نامعتبر وجود داشته باشد؛ export حافظهٔ bounded داشته باشد.

- [ ] **IND-059 · P2 · L — ماتریس پلتفرم و سازگاری binary قابل پشتیبانی**
  شواهد: مشاهده E17؛ Linux x86_64 و CPython 3.10 دامنهٔ فعلی است.
  معیار پذیرش: نسخه‌های فعال CPython، Linux distro/glibc و معماری‌های منتخب در CI واقعاً build/install/test شوند؛ برنامهٔ پایان پشتیبانی و wheel/native ABI مشخص باشد؛ Windows/macOS بدون تست وعده داده نشوند.

- [ ] **IND-060 · P2 · L — مرزبندی و modularization هسته**
  شواهد: DocEngine.cpp بزرگ و ترکیب catalog/index/mutation/query/maintenance در یک مسیر نگهداری است.
  معیار پذیرش: extraction مرحله‌ای با contract test و بدون بازنویسی یک‌باره انجام شود؛ storage، transaction، execution و transport interface مشخص داشته باشند؛ دسترسی مستقیم به DB و raw pointer محدود و مستند شود.

- [ ] **IND-061 · P2 · M — error/protocol/catalog compatibility و deprecation policy**
  شواهد: E10/E11/E17؛ format version وجود دارد، ولی قرارداد همهٔ لایه‌ها باید همخوان باشد.
  معیار پذیرش: client قدیمی capability جدید را تشخیص دهد؛ unsupported field/type خطای پایدار بدهد؛ native/Python mismatch fail-closed باشد؛ fixture نسخه‌های منتشرشده برای upgrade/rollback نگهداری شود.

- [ ] **IND-062 · P2 · M — راهنمای عملی و تجربهٔ توسعه‌دهندهٔ قابل اعتماد**
  شواهد: E18 و مثال‌های چند نسل در README/docs.
  معیار پذیرش: نمونه‌های نصب، schema، transaction، خطا، backup، metrics و shutdown در CI اجرا شوند؛ مورد fake/demo واضح باشد؛ data contract، محدودیت و مسیر عیب‌یابی از یک مرجع تولید شوند.

- [ ] **IND-063 · P2 · S — رفع تناقض مجوز و ادعاهای محصول**
  شواهد: مشاهده E18؛ README=MIT در مقابل LICENSE/pyproject=Apache-2.0.
  معیار پذیرش: تصمیم حقوقی مالک پروژه در همهٔ artifactها یکسان شود؛ NOTICE و مجوز dependencyها بررسی شوند؛ ادعای ACID/live-sync/production دقیقاً به تضمین اثبات‌شده محدود شود. پیش از انتشار تجاری این مورد شرط انتشار است، حتی اگر کوچک باشد.

- [ ] **IND-064 · P2 · L — threat model، بازبینی مستقل و فرآیند پاسخ به آسیب‌پذیری**
  شواهد: hardening و تست داخلی، security certification نیست.
  معیار پذیرش: مرز OS owner/embedded code/API/admin/backup/dependency مشخص شود؛ external review با گزارش رفع یافته‌ها، disclosure policy، CVE/advisory و SLA وصله وجود داشته باشد؛ secret scanner و static analysis پوشش معنادار داشته باشند.

## P3 — HA و توسعهٔ بازار؛ بعد از پایهٔ قابل اعتماد

این اولویت به معنی کم‌اهمیت‌بودن HA نیست. اگر محصول از ابتدا SLA چندنودی/zero-data-loss می‌فروشد، IND-065 تا IND-068 **پیش‌نیاز همان انتشار** هستند. تک‌نودی سالم و backup، معادل failover نیستند.

- [ ] **IND-065 · P3/HA · XL — طراحی replication روی log مشترک و هویت commit**
  شواهد: شکاف E20. پیش از انتخاب فناوری، consistency، sync/async acknowledgment و source of truth مشخص شود.
  معیار پذیرش: document/index/catalog/outbox/receipt یک lineage داشته باشند؛ log محلی گراف یا کپی SST به‌اشتباه consensus نامیده نشود؛ specification و prototype با failure history معتبر ارائه شود.

- [ ] **IND-066 · P3/HA · XL — follower، initial snapshot و catch-up ایمن**
  شواهد: وابسته به IND-065 و IND-048.
  معیار پذیرش: replica از checkpoint معتبر تا sequence جدید catch up کند؛ gap، corruption، backpressure و lag قابل مشاهده باشند؛ stale/read-your-writes policy در client/session روشن باشد.

- [ ] **IND-067 · P3/HA · XL — failover، fencing و جلوگیری از split brain**
  شواهد: شکاف E20. leader lease بدون fencing/epoch برای جلوگیری از writer قدیمی کافی نیست.
  معیار پذیرش: network partition، pause، clock skew و برگشت leader قدیمی دو writer معتبر نسازند؛ quorum، membership و recovery مستند و با history checker تست شوند؛ هر RPO>0 صریح باشد.

- [ ] **IND-068 · P3/HA · L — عملیات cluster و backup در حضور failover**
  شواهد: وابسته به IND-065 تا IND-067؛ اپراتور نباید backup یا rolling upgrade ناسازگار بگیرد.
  معیار پذیرش: rolling upgrade، replica replacement، backup قابل اعتماد از عضو مجاز و restore lineage تمرین شوند؛ dashboard/CLI نقش و lag واقعی را نشان دهند؛ orchestration بدون ادعای تضمین کاذب باشد.

- [ ] **IND-069 · P3 · XL — partition/sharding فقط با قرارداد توزیع روشن**
  شواهد: شکاف E20؛ بعد از تک‌نودی و HA، نه به‌عنوان میان‌بر سرعت.
  معیار پذیرش: shard key، rebalance، hotspot، cross-shard query/constraint/transaction، global ID و graph-cut semantics مشخص باشند؛ داده حین جابه‌جایی گم یا دو بار authoritative نشود.

- [ ] **IND-070 · P3 · L — انتخاب مزیت رقابتی متمایز بر اساس مشتری**
  شواهد: هدف «جایگزین همهٔ دیتابیس‌ها» معیار قابل تحویل نیست. فرضیهٔ مناسب: document+graph تک‌نودی کم‌حافظه با projection قابل بازیابی.
  معیار پذیرش: ۲–۳ کاربرد هدف، dataset واقعی/بازنمایی‌شده و دلیل خرید تعریف شود؛ full-text/geo/vector یا افزونه فقط با نیاز و budget مشخص وارد برنامه شود؛ ML از محدودهٔ این سند خارج است.

- [ ] **IND-071 · P3 · M — اکوسیستم پشتیبانی و تعهد محصول**
  شواهد: رقابت واقعی فراتر از تعداد feature یا یک microbenchmark است.
  معیار پذیرش: support matrix، نسخه‌های پشتیبانی‌شده، policy وصله/ارتقا، incident template، runbook، مستندات اپراتور و کانال گزارش مشکل منتشر شوند؛ SLA فقط برای profile آزموده‌شده تعهد شود.

## ترتیب اجرای پیشنهادی و dependencyها

1. **بستهٔ صحت و حفاظت فوری:** IND-007/008/013/014/015/016/017/018 و reproduction برای IND-005/011/012. کار کم‌هزینهٔ IND-063 نیز پیش از انتشار انجام شود.
2. **ستون فقرات داده:** IND-001 → IND-002/004/006/009؛ IND-003 و IND-033 برای rebuild؛ IND-010 برای قرارداد تراکنش. تا این مرحله ادعای همگامی تضمین‌شدهٔ سند و گراف ممنوع بماند.
3. **تک‌نودی قابل ارائه:** IND-019 تا IND-047 با اولویت workload مشتری؛ correctness با IND-045/046 و ارزش عملکردی با IND-047 تأیید شود.
4. **عملیات و جذب مشتری:** P2 مطابق نیاز مشتری و حجم داده؛ PITR روی ستون فقرات log بنا شود، نه workaround کپی فایل.
5. **چندنودی:** IND-065 → IND-066 → IND-067 → IND-068؛ سپس ارزیابی IND-069. انتخاب الگوریتم consensus بدون specification و تست failure شروع نشود.

برای هر تسک قبل از اجرا owner، milestone و لینک issue ثبت شود. یک تسک XL باید به چند issue کوچک‌تر شکسته شود؛ تیک parent تنها بعد از معیار پذیرش end-to-end زده شود.

## معیارهای خروج محصول

| مرحله | شواهد لازم برای ادعا |
|---|---|
| تک‌نودی امن برای پایلوت | P0 مربوط به workload بسته؛ قرارداد durability/isolation/security مشخص؛ backup/restore و integration واقعی پاس؛ unsupportedها fail-closed |
| رقیب معتبر در یک کاربرد | معیارهای قبلی + subset زبان موردنیاز + عملکرد/هزینه در IND-047 + پشتیبانی و migration مستند؛ نه صرفاً سرعت insert |
| سرویس دارای HA | معیارهای قبلی + replication/fencing/failover و RPO/RTO اثبات‌شده؛ backup آفلاین یا RocksDB WAL کافی نیست |
| ادعای جایگزینی یک محصول خاص | matrix قابلیت و semantics نسخهٔ مشخص آن محصول، مهاجرت آزموده، workload و هزینهٔ قابل مقایسه؛ نبود قابلیت با شفافیت اعلام شود |

**تعریف Done مشترک:** spec و threat/resource contract، تست native و API واقعی، تست failure مناسب، benchmark قبل/بعد در صورت اثر عملکردی، migration/rollback در صورت تغییر format، docs و required CI؛ بدون افزایش خاموش تضمین‌ها در README.

## پیش‌نیازهای تکمیلی عرضه به شرکت‌ها — IND-072 تا IND-091

این بخش featureهای قبلی را دوباره درخواست نمی‌کند؛ آن‌ها را به شواهد، مسئولیت و تصمیم انتشار تبدیل می‌کند. برچسب **P1/GA** یعنی شرط عرضهٔ عمومی شرکتی در profile مربوط؛ حتی اگر اندازهٔ کار کوچک باشد نمی‌توان با عنوان «بعداً انجام می‌دهیم» از آن گذشت. owner مسئول ذخیره‌سازی، امنیت، عملیات و محصول باید مشخص باشد؛ یک نفر می‌تواند چند نقش داشته باشد، ولی ظرفیت واقعی تیم باید با تعهد فروش همخوان باشد.

- [ ] **IND-072 · P1/GA · M — تعریف رسمی Supported Production Profile**
  خروجی: کاربرد مجاز، تک‌نودی/HA، single-tenant، APIهای مجاز، نسخهٔ OS/Python، filesystem، سخت‌افزار، durability، اندازهٔ داده و محدودیت query.
  معیار پذیرش: یک فایل versioned با نام و شناسهٔ profile منتشر شود؛ قابلیت خارج از آن fail-closed یا به‌وضوح غیرفعال باشد؛ عبارت کلی «مناسب همهٔ شرکت‌ها» استفاده نشود. وابسته به IND-070.

- [ ] **IND-073 · P1/GA · M — پروندهٔ شواهد انتشار قابل ممیزی**
  خروجی: commit/tag، hash artifact، SBOM، build/dependency config، profile، test/benchmark report، fixture و known issues در یک release manifest.
  معیار پذیرش: برای هر ادعای correctness/durability/performance/security، لینک شواهد همان نسخه وجود داشته باشد؛ تستِ نسخهٔ قبلی یا fake به‌عنوان اثبات نسخهٔ فعلی پذیرفته نشود. وابسته به IND-044/045/046/047.

- [ ] **IND-074 · P1/GA · L — پذیرش واقعی با شرکت آزمایشی و سناریوی کسب‌وکار**
  خروجی: برنامهٔ pilot با دادهٔ مجاز/ناشناس‌شده، queryهای واقعی، خطاهای قابل انتظار و معیار قبول/رد از قبل.
  معیار پذیرش: حداقل یک تیم مصرف‌کنندهٔ مستقل، نصب و سناریوهای اصلی را بدون تغییر دستی کد هسته تکرار کند؛ نتیجه با سیستم مرجع تطبیق داده شود؛ مالک کاربرد پذیرش را ثبت کند. برای کاربرد یا پلتفرم متفاوت، پذیرش قبلی کافی نیست.

- [ ] **IND-075 · P1/GA · M — SLO و قرارداد خدمت قابل اندازه‌گیری**
  خروجی: latency percentile، throughput، error budget، availability، RPO/RTO و زمان پاسخ پشتیبانی برای همان profile و workload.
  معیار پذیرش: مقادیر قبل از فروش تعیین و با شواهد تأیید شوند؛ دامنهٔ اندازه‌گیری، maintenance و failure mode روشن باشد؛ backup، availability یا zero-data-loss یکدیگر فرض نشوند؛ عدد تبلیغاتی بدون شاهد وارد SLA نشود.

- [ ] **IND-076 · P1/GA · M — ظرفیت مجاز و مرز شکست کنترل‌شده**
  خروجی: جدول node/edge/document/index/concurrency، RAM/disk حداقل و حداکثر آزموده‌شده و headroom لازم.
  معیار پذیرش: نزدیک مرز ظرفیت، overload با backpressure/خطای روشن تمام شود، نه OOM/فساد؛ full disk، pin شدن snapshot، compaction و auth load در ظرفیت‌سنجی لحاظ شوند؛ حد پشتیبانی با حداکثر ID نوع داده اشتباه نشود.

- [ ] **IND-077 · P1/GA · M — معماری نصب مرجع و تأیید persistence سخت‌افزار/فایل‌سیستم**
  خروجی: deployment guide برای مسیر داده، mount، container volume، permission، resource limit، TLS proxy، backup و shutdown.
  معیار پذیرش: خطرهای ephemeral volume، filesystem/mount پشتیبانی‌نشده، write cache و عدم رعایت sync مشخص شوند؛ fsync/POSIX test معادل آزمون قطع برق فیزیکی معرفی نشود؛ هر آزمون مخرب فقط روی محیط دورریختنی و با مجوز اجرا شود. وابسته به IND-008/043/059.

- [ ] **IND-078 · P1/GA · M — نصب و ارتقای نسخه در محیط تمیز مشتری**
  خروجی: بستهٔ release و دستورالعمل نصب آنلاین/آفلاین موردنیاز، بدون dependency پنهان روی workstation سازنده.
  معیار پذیرش: روی ماشین تمیزِ profile، install/start/health/read/write/backup/upgrade انجام شود؛ artifact امضاشده و version mismatch قابل تشخیص باشد؛ فایل binary قدیمی با Python جدید مخلوط نشود.

- [ ] **IND-079 · P1/GA · L — برنامهٔ cutover و برگشت مشتری**
  خروجی: migration rehearsal، توقف/هماهنگی writerها، تطبیق داده و constraint، نقطهٔ تصمیم و rollback plan.
  معیار پذیرش: پیش از cutover، رکورد/رابطهٔ گم‌شده یا تکراری، type loss و schema mismatch گزارش شود؛ rollback به دادهٔ قدیمی، اثر writeهای جدید را بی‌توضیح دور نریزد؛ dual-write بدون پروتکل reconciliation توصیه نشود. ابزار فنی IND-058 جای این پذیرش عملیاتی را نمی‌گیرد.

- [ ] **IND-080 · P1/GA · M — بازیابی توسط اپراتور غیرنویسندهٔ کد**
  خروجی: اجرای restore از backup مستقل روی مقصد تازه توسط فردی غیر از سازنده، همراه log و زمان‌بندی واقعی مراحل.
  معیار پذیرش: داده، graph، catalog، receipt و کلیدهای لازم در محدودهٔ profile بازیابی شوند؛ queryهای کاربرد نتیجهٔ درست بدهند؛ RPO/RTO هدف پاس شود؛ «فایل backup ساخته شد» معیار قبول نباشد. وابسته به IND-009/048/050 و profile مورد عرضه.

- [ ] **IND-081 · P1/GA · M — تحویل runbook و آموزش بهره‌برداری**
  خروجی: راهنمای کمبود دیسک/حافظه، latency، corruption، lock، token/key، maintenance و escalation.
  معیار پذیرش: اپراتور با دسترسی معمول، مشکل شبیه‌سازی‌شده را تشخیص دهد و بدون حذف data directory یا دستور تخریبی نامحدود به مسیر امن برگردد؛ فرمان‌ها در نسخهٔ تحویلی معتبر باشند.

- [ ] **IND-082 · P1/GA · M — تیم پاسخ‌گویی incident و ظرفیت پشتیبانی**
  خروجی: مسئول primary/backup، کانال گزارش، severity، escalation، ارتباط با مشتری و الگوی postmortem.
  معیار پذیرش: مانور incident نشان دهد در زمان تعهدشده فرد مسئول پاسخ می‌دهد؛ تضمین 24×7 بدون ظرفیت واقعی داده نشود؛ برای فساد داده و آسیب‌پذیری، تصمیم توقف rollout و اطلاع‌رسانی روشن باشد. تکمیل سند با وجودنداشتن تیم معادل Done نیست.

- [ ] **IND-083 · P1/GA · M — دسترسی امن پشتیبانی و diagnostic bundle کم‌خطر**
  خروجی: جمع‌آوری config/metrics/version و خطا با redaction، دسترسی موقت و ثبت‌شده و رضایت مشخص مشتری.
  معیار پذیرش: diagnostic پیش‌فرض document، query literal، password، bearer و key را ارسال نکند؛ آپلود خودکار دادهٔ مشتری opt-in باشد؛ پشتیبانی برای بررسی معمول به دریافت کل backup یا secret نیاز نداشته باشد.

- [ ] **IND-084 · P1/GA · L — بررسی الزامات داده و امنیتِ بازار هدف**
  خروجی: طبقه‌بندی داده، محل نگهداری/انتقال، retention، حذف و پایان همکاری؛ الزامات قراردادی/بخشی/کشوری توسط متخصص مربوط بررسی شوند.
  معیار پذیرش: requirement-to-control mapping و شواهد برای مشتری هدف وجود داشته باشد؛ حذف/retention در backup، log، receipt و diagnostics هم لحاظ شود؛ از روی کد یا checklist، ادعای خودکار GDPR/ISO/SOC 2 یا صلاحیت دادهٔ مالی/سلامت نشود. این تسک مشاوره یا تأیید حقوقی محسوب نمی‌شود.

- [ ] **IND-085 · P1/GA · M — بستهٔ مجوز و قرارداد عرضه با بازبینی متخصص**
  خروجی: مجوز یکسان محصول، third-party notices، شرایط پشتیبانی، دامنهٔ تضمین و مسئولیت، مسیر خروج/تحویل داده و سیاست پایان پشتیبانی.
  معیار پذیرش: تناقض IND-063 رفع و شرایط واقعی محصول با قرارداد همخوان باشد؛ تعهدی خارج از capability آزموده‌شده پذیرفته نشود؛ انتشار تجاری و الزامات dependencyها با بازبینی حقوقی مناسب انجام شود.

- [ ] **IND-086 · P1/GA · M — کانال انتشار، maintenance branch و policy وصله**
  خروجی: تفکیک preview/RC/GA، نسخهٔ پشتیبانی‌شده، بازهٔ به‌روزرسانی امنیتی، changelog و compatibility policy.
  معیار پذیرش: hotfix همان gateهای لازم را بگذراند؛ artifact/tag قابل ردیابی و غیرقابل جایگزینی خاموش باشد؛ تغییر format یا تضمین با patch بی‌توضیح منتشر نشود؛ تغییر classifier از Alpha به Stable به‌تنهایی GA محسوب نشود.

- [ ] **IND-087 · P1/GA · M — مدیریت blocker و ریسک باقیماندهٔ release**
  خروجی: فهرست known issues با severity، دامنهٔ اثر، workaround معتبر و owner.
  معیار پذیرش: P0 قابل دسترس در profile، فساد/گم‌شدن دادهٔ شناخته‌شده و آسیب‌پذیری Critical/High قابل بهره‌برداریِ حل‌نشده، اجازهٔ GA نداشته باشند؛ صرف امضا، این blockerها را بی‌خطر نکند؛ ریسک کم‌خطرتر تاریخ بازبینی و پذیرش مسئول داشته باشد.

- [ ] **IND-088 · P1/GA · M — مالکیت نگهداری و مسیر خروج بدون قفل‌شدن مشتری**
  خروجی: owner هر زیرسیستم، مستند handover، نسخه‌های پشتیبانی‌شده و export قابل استفاده خارج NexoraDB.
  معیار پذیرش: وابستگی به حافظهٔ یک توسعه‌دهنده برای restore/upgrade وجود نداشته باشد؛ داده و schema/propertyهای لازم با فرمت مستند قابل تحویل باشند؛ پایان پشتیبانی از قبل اعلام و migration path ارائه شود.

- [ ] **IND-089 · P1/GA · M — هزینهٔ کل و sizing قابل دفاع برای مشتری**
  خروجی: RAM/CPU/storage/backup، amplification، headroom، عملیات نگهداری و بار پشتیبانی برای profile منتخب.
  معیار پذیرش: هزینه و ظرفیت با اندازه‌گیری همان کاربرد توضیح داده شوند؛ سرعت یک microbenchmark یا استفاده از یک کتابخانه، به صرفه‌جویی کلی نسبت به رقبا تعبیر نشود؛ برآورد برای حجم/سخت‌افزار خارج از شواهد علامت‌گذاری شود.

- [ ] **IND-090 · P1/GA · M — تصمیم نهایی Go/No-Go برای نسخهٔ نامزد**
  خروجی: صورت‌جلسه یا release record با تأیید مسئول ذخیره‌سازی، امنیت، عملیات و محصول روی commit/artifact/profile مشخص.
  معیار پذیرش: gate جدول پایین، شواهد IND-073 و blockerهای IND-087 بازبینی شوند؛ تغییر مهم پس از sign-off بخش‌های اثرپذیرفته را دوباره اعتبارسنجی کند؛ تصمیم برای نسخهٔ قبلی به نسخهٔ جدید منتقل نشود.

- [ ] **IND-091 · P1/GA · M — rollout کنترل‌شده و معیار توقف عرضه**
  خروجی: برنامهٔ انتشار مرحله‌ای، telemetry کم‌خطر، علامت‌های توقف و بازگشت امن یا freeze عملیات.
  معیار پذیرش: فساد، رشد غیرعادی خطا/منابع یا شکست recovery، rollout را متوقف کند؛ downgrade ناسازگار به‌عنوان rollback پیشنهاد نشود؛ ریشه‌یابی و رفع مشکل قبل از ادامه ثبت شود. این تسک جای آزمون‌های چندروزهٔ مستثناشده را نمی‌گیرد.

## آیا بعد از این تسک‌ها می‌توان محصول را به شرکت‌ها عرضه کرد؟

**بله، مشروط به عبور از gateهای زیر برای کاربرد و profile مشخص؛ نه با تضمین مطلق و نه صرفاً با تیک‌زدن فایل.** لازم نیست تمام قابلیت‌های همهٔ رقبا ساخته شوند، اما هر چیزی که به مشتری وعده داده می‌شود باید کامل، آزموده و پشتیبانی‌پذیر باشد. ویژگی مستثناشده یا ناتمام خودکار Done نیست؛ اگر کاربرد مشتری به آن نیاز داشته باشد، آن کاربرد هنوز قابل عرضه نیست.

| دروازه | عرضهٔ مجاز | شروط لازم | ادعای غیرمجاز |
|---|---|---|---|
| G0 — Preview داخلی | تحقیق، توسعه و demo با دادهٔ غیرحیاتی | محدودیت‌ها و خطرها واضح؛ backup و محیط آزمایشی مستقل | database تولیدی با تضمین سلامت داده |
| G1 — Pilot کنترل‌شدهٔ شرکتی | ارزیابی محدود با موافقت و rollback روشن | P0های مربوط بسته؛ IND-072/073/074/075/079/080/081/083/087 تأیید؛ مسئول پاسخ‌گویی مشخص | GA، جایگزینی عمومی رقبا یا SLA تأییدنشده |
| G2 — GA تک‌نودی | عرضهٔ تولیدی به شرکت، فقط در profile تک‌نودی پشتیبانی‌شده | تمام P0های قابل دسترس و P1های لازم برای آن profile بسته؛ P2های موردنیاز مشتری کامل؛ IND-072 تا IND-091 تأیید؛ تصمیم Go ثبت | failover خودکار، RPO=0 یا multi-tenant/feature خارج از profile بدون شواهد |
| G3 — GA با HA | عرضهٔ خدمت دارای تضمین چندنودی | G2 + IND-065 تا IND-068 و RPO/RTO/consistency همین topology اثبات؛ upgrade و failover قابل بهره‌برداری | HA صرفاً به دلیل backup، چند process، Kubernetes یا RocksDB WAL |

**قاعدهٔ معافیت:** تسک خارج از محصول هدف می‌تواند «خارج از محدوده» شود، نه «انجام‌شده». دلیل، owner و اثر روی قرارداد باید ثبت شود؛ مسیر unsupported باید مسدود و تست شود. risk شناخته‌شدهٔ فساد داده یا bypass امنیت در همان profile با یک برچسب out-of-scope قابل چشم‌پوشی نیست. تغییر مشتری، workload یا پلتفرم نیازمند بازبینی profile است.

### قالب پروندهٔ Go/No-Go

```text
نسخه / commit / artifact hash:
نام و نسخهٔ production profile:
کاربردهای مجاز و موارد غیرقابل پشتیبانی:
محیط / سخت‌افزار / filesystem / durability:
سقف داده / concurrency / منابع:
SLO و دامنهٔ سنجش / RPO / RTO:
تسک‌های لازم و لینک شواهد پذیرش:
نتیجهٔ security review و blockerهای باز:
نتیجهٔ نصب تمیز / پذیرش مشتری / restore مستقل:
برنامهٔ cutover / rollback یا freeze امن:
مسئول پشتیبانی و مسیر escalation:
ریسک‌های پذیرفته‌شده / موارد خارج از محدوده:
تأیید storage / security / operations / product:
تصمیم Go یا No-Go / تاریخ / شروط بازبینی:
```

## منابع مرجع صنعتی

این منابع برای استخراج قرارداد و معیار پذیرش‌اند، نه ادعای برتری NexoraDB یا الزام تقلید تمام قابلیت‌ها. مقایسه باید نسخه/edition مشخص داشته باشد.

- [PostgreSQL: standby و replication](https://www.postgresql.org/docs/current/warm-standby.html): تفکیک recovery و HA و acknowledgment replication.
- [MongoDB: isolation/consistency/recency](https://www.mongodb.com/docs/manual/core/read-isolation-consistency-recency/): سطح تضمین read/write و تراکنش را صریح بیان می‌کند.
- [SQL Server Audit](https://learn.microsoft.com/en-us/sql/relational-databases/security/auditing/sql-server-audit-database-engine?view=sql-server-ver17): audit مستقل و قابل مدیریت، فراتر از query log.
- [Neo4j clustering architecture](https://neo4j.com/docs/operations-manual/current/clustering/introduction/): قابلیت چندنودی و نقش اعضا، نه صرفاً اجرای algorithm گراف.
- [سند وضعیت فعلی NexoraDB](cpp/DATA_PATH_AUDIT.md) و [قرارداد cursor/pool/ACL](cpp/CURSORS_POOL_ACL.md): مرجع داخلی محدودیت‌های بررسی‌شده؛ task جدید باید با تغییرات بعدی مخزن reconcile شود.
- [NIST SSDF، SP 800-218](https://csrc.nist.gov/pubs/sp/800/218/final): مرجع فرآیند توسعه و تحویل نرم‌افزار امن؛ دنبال‌کردن آن به‌تنهایی گواهی runtime correctness دیتابیس نیست.
- [OWASP ASVS](https://asvs.owasp.org/): مرجع requirements و verification لایهٔ وب/API/admin؛ جای آزمون تراکنش، recovery یا consensus هسته را نمی‌گیرد.
