# MEM-001 — مالکیت حافظه و schema telemetry

گزارش حافظه سه دید جدا دارد: صفحات resident از سیستم‌عامل، درخواست‌ها/ظرفیت‌های داخلی موتور، و وضعیت allocator/Python.
هیچ‌یک از جمع‌زدن همهٔ این دیدها به RSS تبدیل نمی‌شود. مقدار نامعلوم `null` است؛ صفر فقط برای صفرِ مشاهده‌شده استفاده می‌شود.

## ورودی‌ها و نسخه‌ها

```python
import json
import nexoradb
from nexoradb.memory import collect_memory

sample = collect_memory([engine], [manager], native=nexoradb)
print(json.dumps(sample, indent=2))
```

collector از package پایتون، با native هم‌نسخه، قابل استفاده است. آرگومان‌های engines/managers اختیاری‌اند؛
پس از destroy نیز با `native=nexoradb` شمارندهٔ process-wide snapshot قابل خواندن است.
caller فهرست ownerهای مورد بررسی را می‌دهد؛ کشف خودکار تمام engine/managerهای process وجود ندارد.
shared cache/WBM ممکن است ownerهای دیگری هم داشته باشند؛ subtotal فقط پوشش اعلام‌شدهٔ sample است.
گزارش کامل مطابق [schema JSON نسخهٔ ۱](../telemetry/memory-v1.schema.json) است.

| API | نسخه و کاربرد |
| --- | --- |
| `DocEngine.memory_report()` | schema ۲؛ کلیدهای قبلی حفظ شده‌اند، accounting و شناسهٔ منبع مشترک اضافه شده است |
| `GraphManager.memory_report()` | schema ۱؛ اجزای LiveGraph، snapshot cache، scheduler و قواعد جمع |
| `nexoradb.graph_snapshot_memory()` | درخواست‌های PMR زنده/peak و تعداد resourceهای زنده در کل native module |
| `StaticGraph.owned_allocation_bytes()` | درخواست‌های متعلق به همان snapshot، بدون baseline پذیرش clone |
| `nexoradb.memory.collect_memory(...)` | ترکیب دیدهای یک process، dedup و reconciliation با دامنهٔ صریح |

گزارش `pid`، زمان monotonic شروع و هزینهٔ sample را ثبت می‌کند. قفل‌ها و شمارنده‌ها به‌ترتیب خوانده می‌شوند؛
این تصویر اتمیک از تمام process نیست. ساخت گزارش خودش allocation دارد و بخشی از حافظهٔ مشاهده‌گر است.
گزارش LiveGraph ممکن است adjacency را پیمایش کند؛ collector ابزار کم‌بسامد تشخیصی است و به مسیر هر query وصل نشده است.
collector، tracing/GC/flush/trim را فعال نمی‌کند. گزارش native طبق رفتار موجود می‌تواند cursor idle منقضی را پاک کند؛ این اثر در accounting ثبت شده است.

## schema یک measurement

هر ردیف `measurements` شامل `id`، `value`، `owner`، `unit`، `measurement`، `overlaps` و `reason` است.
واحد `bytes` یا `count` است. نوع اندازه‌گیری یکی از موارد زیر است:

| نوع | تفسیر |
| --- | --- |
| `counter` | مقدار گزارش‌شدهٔ API یا درخواست‌های allocator ابزارگذاری‌شده؛ الزاماً اندازهٔ اشیای زنده یا resident نیست |
| `estimate` | تخمین ظرفیت/ساختار؛ overhead و rounding allocator ممکن است خارج باشد |
| `capacity` | سقف تنظیم‌شده؛ مصرف فعلی نیست |
| `reservation` | charge پذیرش کار؛ allocation واقعی نیست |
| `high_water` | بیشینهٔ تاریخی؛ با current جمع نمی‌شود |
| `unavailable` | مقدار `null` همراه دلیل؛ مقدار صفر یا حدس جایگزین نشده است |

شناسهٔ `rocks_resource:<id>` در طول عمر منبع و در همان process یکتا است؛ برای ترکیب processها باید `pid` نیز کلید باشد.
engine و managerهای تکراری در آرگومان‌ها با identity حذف می‌شوند. اندیس‌های document/manager/graph به raw همان sample اشاره دارند؛
نام گراف در raw حفظ می‌شود و این اندیس‌ها شناسهٔ دائمی گراف در سری زمانی نیستند.

## مالکیت، هم‌پوشانی و شکاف‌ها

| حوزه | آنچه اندازه‌گیری می‌شود | قاعده و شکاف |
| --- | --- | --- |
| block cache | usage، pinned، capacity | pinned زیرمجموعهٔ usage است؛ index/filter blockها داخل cache هستند؛ shared RocksResources فقط یک‌بار شمرده می‌شود |
| memtable/WBM | budget و `memory_usage()` منبع مشترک | requested/arena accounting است؛ per-DB memtable properties روی همین حافظه هم‌پوشانی دارند |
| active memtable | `cur-size-active-mem-table` | زیرمجموعهٔ active+unflushed است |
| all memtables | `cur-size-all-mem-tables` | تخمین active و immutableهای flush‌نشده |
| retained memtables | `size-all-mem-tables` | active، flush‌نشده و immutableهای pinned را شامل می‌شود؛ با دو مورد قبلی جمع نمی‌شود |
| table readers | `estimate-table-readers-mem` | طبق header RocksDB 8.5.3 حافظهٔ داخل block cache را مستثنا می‌کند؛ همچنان تخمین است |
| document snapshots | تعداد RocksDB snapshot و page session/live/retired/active/idle | per-snapshot byte attribution موجود نیست؛ page snapshot نسخه و memtable موجود را نگه می‌دارد، حافظهٔ جداگانهٔ قابل جمع ندارد |
| document queries | `active_query_executions` | دامنهٔ ExecuteQuery؛ point/page/transaction و buffer/result bytes پوشش کامل ندارند؛ شمارندهٔ صفر به معنی آزادشدن result متعلق به caller نیست |
| metadata/transaction | تعداد collectionهای cache؛ byte fields برابر null | ظرفیت map/stringها و write-batch/transaction buffers هنوز درخواست‌به‌درخواست ابزارگذاری نشده‌اند |
| LiveGraph | node/edge/adjacency/identity/type/WAL/storage estimates | total تخمینی خودش شامل اجزای زیرین است؛ componentها دوباره با total جمع نمی‌شوند؛ manager/GraphIdStore maps و allocator overhead کامل نیستند |
| snapshotهای گراف | PMR current/peak و resource count | تمام managerها، cacheها، cloneهای بیرونی و scratch ساخت snapshot؛ requested bytes موفق، بدون baseline پذیرش؛ wrapperها و allocationهای غیر-PMR خارج‌اند |
| snapshot cache | owned requested bytes هر cache | زیرمجموعهٔ PMR process-wide است؛ دوباره شمرده نمی‌شود |
| scheduler/query گراف | queued/running و reserved bytes | reservation با snapshot/workspace هم‌پوشان است؛ workspace/result الگوریتم‌ها اندازه‌گیری کامل ندارند |
| Python | allocated-block count؛ tracemalloc current/peak/overhead در صورت tracing موجود | block count بایت نیست؛ tracing فقط تخصیص‌های قابل ردیابی از زمان شروع را می‌بیند؛ تمام Python یا C++ نیست |
| glibc | arena، allocated/free arena، mmap و keepcost | process-wide؛ Python/native مشترک‌اند؛ pymalloc/tcache، mmap مستقیم و allocator دیگر attribution دقیق ندارند |
| OS | RSS/PSS، private/shared، anonymous، PSS anon/file/shmem، swap | از یک خواندن smaps_rollup؛ resident با requested/capacity یکی نیست؛ دادهٔ فایل روی دیسک یا page cache نامپ‌شده، RSS process نیست |

در پیکربندی فعلی، WBM با `cache=nullptr` ساخته می‌شود و به block cache charge نمی‌کند؛ این فرض در native accounting آمده است.
منابع دیگر یا نسخهٔ دیگر RocksDB نباید بدون بازبینی همان قاعدهٔ جمع را به ارث ببرند.
`allocation_current_bytes()` قدیمی clone شامل baseline cache برای admission است؛ برای attribution از آن استفاده نمی‌شود.
شمارندهٔ جدید global PMR فقط allocation واقعی هر resource را ثبت می‌کند؛ baseline به عنوان تخصیص دوباره اضافه نمی‌شود.

## تطبیق با RSS/PSS

در Linux، چهار ستون private/shared clean/dirty همان rollup با RSS تطبیق داده می‌شوند:

```text
RSS = Private_Clean + Private_Dirty + Shared_Clean + Shared_Dirty
```

اختلاف در `process.rss_partition_delta_bytes` ثبت می‌شود. PSS سهم کسری صفحه‌های مشترک است؛
RSS و PSS را نباید جمع کرد. ستون‌های PSS anon/file/shmem ممکن است به‌علت rounding چند KiB اختلاف داشته باشند.
مقدار unavailable/permission denied با `null` و `errors` ثبت می‌شود؛ Linux/PSS روی همهٔ سیستم‌ها فرض نشده است.

`reconciliation.selected_native_estimate_bytes` فقط subtotal تشخیصی زیر است:

```text
یک usage برای هر cache مشترک + یک usage برای هر WBM مشترک
+ table-reader estimate هر DB + total estimate هر LiveGraph
+ current requested PMR تمام snapshotها، فقط یک‌بار
```

capacity، pinned subset، aliasهای memtable، cached PMR، reservation، Python traces و allocator totals از این subtotal خارج‌اند.
اگر یکی از contributionهای مورد انتظار unavailable باشد، subtotal و اختلاف‌ها null می‌شوند.
دو مقدار `rss_minus_selected_native_estimates_bytes` و `pss_minus_selected_native_estimates_bytes` **اختلاف علامت‌دار تشخیصی** هستند؛
«resident بدون مالک» یا بایت leak محسوب نمی‌شوند و می‌توانند منفی شوند. allocation درخواست‌شده ممکن است resident نباشد؛
RSS شامل runtime، stack/thread، code/library، Python، صفحات آزاد allocator و mmapهای دیگر است.

برای جلوگیری از total جعلی، `process_total_internal_bytes` در schema الزاماً `null` است.
`missing_attribution` شکاف‌های query/result/transaction، metadata/manager maps، rounding/tcache، Python untraced و stack/code/mmap را مشخص می‌کند.
حتی subtotal صفر پس از destroy به معنی RSS صفر نیست.

## شواهد و حدود پذیرش

آزمون‌های محلی ۲۰۲۶-۱۰-۳۰:

- ۷ تست Python مالکیت/گزارش: dedup دو engine با منبع مشترک، unavailable بدون صفر جعلی، schema، RSS partition، عدم GC/tracing ضمنی و آزادشدن cache/clone/external snapshot.
- ۲۸ تست native document و ۲۳ تست native graph مرتبط موفق؛ تست PMR جداگانه allocation ناموفق و baseline را از درخواست واقعی تفکیک می‌کند.
- ۲۵ تست lab موفق: ۲۲ unit و سه protocol واقعی native روی store خصوصی کوچک؛ stageها بدون trim خودکار آزموده شدند. integration مشترک MongoDB/PostgreSQL اجرا نشده است.
- [آزمایش چرخه‌ای و نتیجهٔ retention](MEMORY_RETENTION_FA.md) شامل RSS/PSS و تمام raw componentها است.
- ۲۲۹۲ sample از دو workload Release: اختلاف RSS partition صفر و PMR پس از destroy در تمام ۲۴۰ چرخه صفر؛ report و hash source/native حفظ شده‌اند.

گزارش حافظه، بودجهٔ مشترک enforce‌شدهٔ MEM-003، tracking کامل scratch، اثبات نبود leak، یا پذیرش ۱۰M/۱۰۰M در MEM-007 نیست.
محدودیت اندازه‌گیری هر حوزه در schema و این سند حفظ شده است؛ دادهٔ نامعلوم با یک جمع ظاهراً دقیق پنهان نمی‌شود.
