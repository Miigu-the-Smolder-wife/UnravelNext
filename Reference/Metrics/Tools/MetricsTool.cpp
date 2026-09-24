// unx_metrics: compares a test image against a reference (INTERFACES_KO.md 10.3).
//   unx_metrics --reference ref.pfm --test test.pfm [--ldr] [--out report.json] [--error-map map.pfm]
//   unx_metrics --temporal f0.pfm f1.pfm ... [--out report.json]
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/metrics/Metrics.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace unx;

int main(int argc, char** argv)
{
    try
    {
        std::string ref, test, out, errorMap;
        bool ldr = false, temporal = false;
        std::vector<std::string> frames;
        for (int i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--reference") ref = next();
            else if (a == "--test") test = next();
            else if (a == "--ldr") ldr = true;
            else if (a == "--out") out = next();
            else if (a == "--error-map") errorMap = next();
            else if (a == "--temporal") temporal = true;
            else if (temporal) frames.push_back(a);
            else fail("unknown argument %s", a.c_str());
        }
        std::string json;
        if (temporal)
        {
            std::vector<metrics::Image> images;
            for (const auto& f : frames) images.push_back(metrics::readPfm(f));
            std::vector<double> v = metrics::temporalInstability(images);
            double mean = 0;
            for (double x : v) mean += x;
            mean = v.empty() ? 0 : mean / v.size();
            logf("temporal instability: mean %.6g over %zu frame pairs\n", mean, v.size());
            json = format("{\"temporal_instability_mean\": %.9g, \"pairs\": %zu}\n", mean, v.size());
        }
        else
        {
            if (ref.empty() || test.empty()) fail("--reference and --test are required");
            metrics::Image r = metrics::readPfm(ref), t = metrics::readPfm(test);
            const double rel = metrics::relMse(r, t);
            metrics::FlipResult f = ldr ? metrics::flipLdr(r, t) : metrics::flipHdr(r, t);
            logf("relMSE %.6g, %s-FLIP mean %.6g, p99 %.6g (%ux%u)\n", rel, ldr ? "LDR" : "HDR", f.mean, f.p99, r.width, r.height);
            json = format("{\"relmse\": %.9g, \"flip_mode\": \"%s\", \"flip_mean\": %.9g, \"flip_p99\": %.9g, \"width\": %u, \"height\": %u}\n", rel, ldr ? "ldr" : "hdr",
                          f.mean, f.p99, r.width, r.height);
            if (!errorMap.empty())
            {
                metrics::Image m;
                m.width = r.width;
                m.height = r.height;
                m.rgb.resize(f.errorMap.size() * 3);
                for (size_t i = 0; i < f.errorMap.size(); ++i) m.rgb[3 * i] = m.rgb[3 * i + 1] = m.rgb[3 * i + 2] = f.errorMap[i];
                metrics::writePfm(errorMap, m);
            }
        }
        if (!out.empty()) writeTextFile(out, json);
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
