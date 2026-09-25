#pragma once
// Thin-film optics and colour for the material v2 studies (Docs/Design/MATERIAL_LAYERS_KO.md 1.2, 3).
//   reference  exact polarised Airy reflectance per wavelength (360-830 nm, 1 nm; complex Fresnel, n + ik), CIE 1931
//              2 deg, illuminant E, linear Rec.709 white-balanced to E (constant reflectance R -> RGB (R, R, R))
//   (a)        Belcour & Barla 2017, revised as the design 1.2 adopts it: Airy intensity series with Gaussian-fit XYZ
//              sensitivity Fourier terms for m <= 3, closed geometric tail for m >= 4 damped with the m = 4 Gaussian,
//              exact complex Fresnel (n + ik), films in total internal reflection evaluated with complex cos (the
//              evanescent film has no interference: its Airy reflectance is integrated over 16 wavenumber bins)
//   (b)        previous engine (TitanNative filmBand): exact Airy averaged over three inverse-wavelength bands
//   (c)        N bins: bin-averaged Airy over each wavenumber bin (Poisson kernel, the (b) formula) weighted by the
//              bin's CMF integrals; the substrate is stored per bin (spectral n, k at the bin centre)
// Substrates carry spectral n, k (reference) and the RGB triplet the renderer stores (point samples or fitted).
#include <array>
#include <complex>
#include <string>
#include <vector>

namespace unx::study
{
using cd = std::complex<double>;
using Rgb3 = std::array<double, 3>;

struct Substrate
{
    std::string name;
    bool spectral = false;
    double n = 1.5, k = 0;
    std::vector<double> wl, sn, sk;  // um
    double rgbN[3] = {}, rgbK[3] = {};
    cd at(double lambdaNm) const;
};
Substrate constantSubstrate(const std::string& name, double n, double k);
// Spectral metal from Reference/Studies/data/<file> (um, n, k); the RGB triplet is the 650/550/450 nm point sample.
Substrate metal(const std::string& name, const std::string& file);
// Replaces the RGB triplet by per-channel (n, k) fitted to the reference reflectance over incidence 0-89 deg in outer
// medium n0 (least squares). Degenerate channels (a valley of equal error) take the smallest k within 0.1 % of the
// best error. Returns the fit's max |dR| over angles per channel.
Rgb3 fitRgb(Substrate& s, double n0 = 1.0);

double airy(double n0, double cos0, double nf, cd n2, double dNm, double lambdaNm);  // exact, s/p averaged
Rgb3 filmReference(double n0, double cos0, double nf, double dNm, const Substrate& s);
Rgb3 filmA(double n0, double cos0, double nf, double dNm, const Substrate& s);
Rgb3 filmB(double n0, double cos0, double nf, double dNm, const Substrate& s);
Rgb3 filmC(double n0, double cos0, double nf, double dNm, const Substrate& s, int bins);

double deltaE76(const Rgb3& a, const Rgb3& b, double whiteY = 1.0);  // linear Rec.709/E values; white = (whiteY)^3
double luminance(const Rgb3& c);
} // namespace unx::study
