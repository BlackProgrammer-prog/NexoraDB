#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <vector>
// Standalone benchmark module, not part of the installed database API.
PYBIND11_MODULE(nexora_conversion_probe,m) {
    m.def("roundtrip_string",[](std::string value) { return value; });
    m.def("make_string",[](size_t bytes) { return std::string(bytes,'x'); });
    m.def("make_strings",[](size_t count,size_t width) { return std::vector<std::string>(count,std::string(width,'x')); });
    m.def("sum_strings",[](const std::vector<std::string>& values) { size_t sum=0; for (const auto& value:values) sum+=value.size(); return sum; });
}
