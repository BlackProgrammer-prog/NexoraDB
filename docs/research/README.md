# مرحلهٔ صفر پژوهش NexoraDB

این مجموعه از ۲۰۲۶-۱۰-۲۸ شروع شده است. پذیرش مرحلهٔ صفر مربوط به **قرارداد پژوهش و ابزار قابل اجرا** است، نه اثبات فرضیه‌ها یا برتری موتور. وضعیت دقیق و شواهد در پروندهٔ وضعیت است.

- [RES-001: workloadهای versioned](PHASE0_WORKLOADS_V1_FA.md)
- [RES-002: ماتریس و منابع رسمی](COMPARISON_MATRIX_V1_FA.md)
- [RES-003: فرضیه‌ها و معیار تصمیم](HYPOTHESES_V1_FA.md)
- [RES-005: ممیزی و قرارداد لایه‌ها](LAYER_ATTRIBUTION_V1_FA.md)
- [وضعیت و شواهد کار](STATUS_PHASE0_FA.md)
- [پذیرش والدهای RES-004/005 و فرمان آزمون طولانی](PARENT_ACCEPTANCE_FA.md)

## RES-004 — plan / run / analyze

دستورها از root پروژه داخل Linux/WSL اجرا می‌شوند؛ بستهٔ native فعلی و driverهای lab لازم‌اند.
`plan` فایل خروجی نمی‌سازد، موتور باز نمی‌کند و دستور driver را اجرا نمی‌کند.

```bash
python3 scripts/research_ab.py plan \
  --config nexora-scale-lab/config/research-smoke-v1.json

python3 scripts/research_ab.py run \
  --config nexora-scale-lab/config/research-smoke-v1.json \
  --directory build/research-smoke-UNIQUE-ID

python3 scripts/research_ab.py analyze \
  --directory build/research-smoke-UNIQUE-ID

python3 -m unittest discover -s tests -p test_research_phase0.py -v
```

config نمونه مسیر interpreter همین workspace را دارد؛ روی host دیگر پیش از run مسیر را صریح عوض کنید.
نسخهٔ config ثابت بماند؛ **A و B نمونه عمداً یک binary هستند**، پس این A/A sanity check است، نه نتیجهٔ بهبود.
برای A/B واقعی دو build جدا، مسیر binaryها و فرمان driver هر arm صریح ثبت شوند. `require_loaded_artifacts=true` در config نمونه، SHA گزارش‌شدهٔ loader را با binary اعلام‌شده تطبیق می‌دهد؛ مسیر native هر arm به process تازه داده می‌شود. هر binary نیز در شواهد ذخیره می‌شود تا با تعویض build فعلی baseline از دست نرود. metadata از driver مورد اعتماد است، نه remote attestation در برابر کد مهاجم.
commandها بدون shell اجرا می‌شوند ولی کد دلخواه کاربر هستند؛ sandbox نیست. secret داخل argv/config نگذارید، چون در manifest ذخیره می‌شود.

اجرای تک‌سناریو برای بررسی صحت (پیش‌فرض ۶۴ سند/۱۰۰ عملیات، trace version2؛ خروجی timing قابل انتشار نیست):

```bash
nexora-scale-lab/.venv/bin/python scripts/research_workload.py --scenario document-oltp
nexora-scale-lab/.venv/bin/python scripts/research_workload.py --scenario document-graph
nexora-scale-lab/.venv/bin/python scripts/research_workload.py --scenario low-memory
```

## قرارداد شواهد و محدودیت ابزار

- سه تا بیست block؛ هر block تصادفیِ seedدار ABA یا BAB؛ process و DB تازه برای هر invocation driver فعلی.
- manifest شامل commit، dirty flag، hash فایل‌های مشمول snapshot، hash binaryهای اعلام‌شده، config، host، affinity و allocator env است. runtime واقعی Python و نسخهٔ driver/dependencyها در خروجی driver ثبت می‌شوند و تغییر آن‌ها بین armها رد می‌شود.
- source/configهای hash‌شده در `source.tar.gz` و binaryهای اعلام‌شده در `binaries/` هم نگهداری می‌شوند؛ بازسازی baseline به commit تنها وابسته نیست. فایل‌های ignored، DB، secretهای env و کل سورس toolchain/vendor به این آرشیو اضافه نمی‌شوند.
- source tree هدف کاندید فعلی است؛ provenance سورس build قدیمیِ A خودکار بازسازی نمی‌شود؛ build manifest مستقل آن باید نگهداری شود. ثبت source snapshot به‌تنهایی ثابت نمی‌کند یک binary خارجی از همان سورس build شده است.
- stdout/stderr و JSON هر sample به صورت exclusive ساخته می‌شوند؛ output directory موجود رد می‌شود. داخل repo فقط مسیر git-ignored مجاز است. baseline و گزارش‌های قبلی overwrite نمی‌شوند.
- timeout هر invocation حداکثر ۶۰۰ ثانیه؛ گروه process همان invocation در پایان/خطا پاک می‌شود، سرویس خارجی مدیریت نمی‌شود. driver باید فقط منابع اختصاصی خودش را لمس کند.
- شکست correctness، NaN/Infinity، شمار عملیات متفاوت، config mismatch یا نمونهٔ ناقص اجازهٔ گزارش معتبر نمی‌دهد.
- completion شامل SHA فایل‌های شواهد و snapshot binaryهاست؛ analyze کمبود یا تغییر شواهد را رد می‌کند. این امضا یا محافظت در برابر مهاجمِ قادر به بازنویسی hash نیست.
- bracket drift در throughput یا p99، یا CV throughput بالاتر از آستانه، نتیجه را نامعین می‌کند. CI95 از percentile bootstrap با ۵۰۰۰ resampling block مستقل و seed مشخص محاسبه می‌شود؛ کمتر از پنج block exploratory است. قبول فرضیه خودکار نیست.
- source و binaryها قبل و بعد هر sample بررسی می‌شوند. candidate باید در طول run ثابت باشد؛ این مکانیزم build hermetic یا محافظت در برابر تغییر و بازگرداندن عمدی فایل در وسط اجرا نیست.
- در پروفایل پذیرش جدید، affinity اعمال و گزارش می‌شود؛ PSI، load، disk counters و governor/frequency (در صورت دسترسی) ثبت می‌شوند. دادهٔ ناموجود WSL صریح است؛ thermal monitoring کامل و attribution داخلی موتور ادعا نمی‌شود. پروفایل smoke قدیمی بدون تنظیم affinity الزاماً آن را اعمال نمی‌کند.

برای افزایش حجم از این smoke استفاده نکنید: guard آن عمداً ۱۰هزار رکورد است.
اجرای ۱۰M/۱۰۰M، نصب رقبا، حذف DB قدیمی و تغییر سرویس‌های سیستم جزو این مرحلهٔ انجام‌شده نیست.

## آزمون مشترک لایه‌ها و محدودیت واقعی حافظه

محیط Python باید dependencies پروژه، `uvicorn` و `httpx` را داشته باشد. مسیر venv آزمایش‌شده و نسخهٔ پکیج‌ها در manifest گزارش ثبت می‌شوند.

```bash
cmake -S . -B build/release -DNEXORA_BUILD_RESEARCH_PROBES=ON
cmake --build build/release --target nexora_research_read_probe -j 2
python scripts/research_layers.py --plan
python scripts/research_layers.py --directory build/layers-UNIQUE-ID

# فقط kernel cgroup موقت برای subprocess جدید؛ root داخل WSL لازم است.
python scripts/research_validate.py --directory build/phase0-UNIQUE-ID --with-cgroup

# پس از run؛ طرح و baseline را بدون اجرای benchmark تازه freeze می‌کند.
python scripts/research_preregister.py \
  --baseline build/phase0-UNIQUE-ID/aa-control \
  --directory build/preregistration-UNIQUE-ID
```

validation شامل دو تکرار هر سه سناریو با ۱/۱۰ کلاینت (۱۰۰۰ سند/۲۰۰ عملیات)، چهار W3 در cgroup مستقل، A/A پنج-block و سه لایهٔ واقعی است؛ هیچ سرویس Mongo/Postgres یا دادهٔ قدیمی لمس نمی‌شود.
نسخه/edition هدف رقبا با نسخهٔ واقعاً نصب‌شده یکی فرض نشده است؛ admission runtime، license review، adapters هم‌معنا و آزمایش‌های اصلی پس از این مرحله لازم‌اند.
