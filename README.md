# SkimNanoAOD

A simple NanoAOD skim code.

## Jet Energy Correction (JEC) and Jet Energy Resolution (JER) strategy

`nano_scan` and `plotMaker` must use exactly the same jet definition.  In
particular, do **not** correct the same NanoAOD jet twice: `Jet_pt` is normally
already the centrally corrected NanoAOD value.  Choose one of the following
models and record the choice in the output metadata.

1. **Use NanoAOD's stored variations (recommended for plotting).**  Read the
   nominal and variation branches produced by the NanoAOD campaign, for example
   `Jet_pt_nom`, `Jet_pt_jesTotalUp`, `Jet_pt_jesTotalDown`, `Jet_pt_jerUp`, and
   `Jet_pt_jerDown` when they exist.  Apply the corresponding branch *before*
   jet selection, sorting, b tagging, mass reconstruction, and MET-dependent
   selections.  Branch availability is campaign-dependent; inspect the input
   schema rather than assuming all names exist.
2. **Recalculate corrections (needed when adding a new payload/source).**
   Start with the uncorrected momentum
   `rawPt = Jet_pt * (1 - Jet_rawFactor)`, apply the complete JEC chain once,
   then apply JER smearing only for simulation.  Use the JERC payload matching
   the exact NanoAOD era, jet collection (usually `AK4PFchs`), data run range,
   and MC/data type.  For data include the appropriate residual correction;
   never apply an MC payload or JER smearing to data.

### `nano_scan`: correction order

The correction calculation needs `Jet_eta`, `Jet_phi`, `Jet_area`,
`Jet_rawFactor`, and the event rho (`fixedGridRhoFastjetAll`), as well as the
event/run/luminosity block and jet index to seed random smearing reproducibly.
The following pseudocode shows the required order (with a text-payload JERC
implementation; the same inputs/order apply to correctionlib JSON payloads):

```cpp
double rawPt = jet.pt * (1.0 - jet.rawFactor);
double jec = corrector(rawPt, jet.eta, jet.area, event.rho); // L1...L3(+Residual on data)
double correctedPt = rawPt * jec;

if (event.isMC) {
  double resolution = jerResolution(correctedPt, jet.eta, event.rho);
  double sf = jerScaleFactor(jet.eta, variation); // nominal, up, or down

  if (jet.hasMatchedGenJet) {
    // Use a gen match only when it satisfies the official JER matching criteria.
    correctedPt = std::max(0.0,
        correctedPt + (sf - 1.0) * (correctedPt - jet.genJetPt));
  } else {
    std::mt19937 rng(seed(event.run, event.lumi, event.event, jet.index));
    double width = resolution * std::sqrt(std::max(sf * sf - 1.0, 0.0));
    correctedPt *= std::max(0.0, 1.0 + gaussian(rng) * width);
  }
}
```

The real implementation must use the JERC-prescribed generator matching
condition (including the angular and resolution criteria), not merely a
non-negative `Jet_genJetIdx`.  Use one deterministic seed per
event/jet/variation so repeated `nano_scan` runs and `plotMaker` agree.
Retain the nominal JEC/JER scale and every requested variation in the skim,
along with enough information to audit the payload version.

If `nano_scan` changes jet transverse momentum, it must also rebuild every
jet-derived quantity.  This includes the selected jet multiplicity, leading
jet ordering, HT, b-tagged-jet list, dijet observables, and Type-1 MET (vector
propagate each accepted jet's corrected-minus-reference momentum).  Keeping the
original NanoAOD MET together with re-corrected jets is inconsistent.

### `plotMaker`: systematic handling

Construct a small jet-variation adapter rather than scattering branch names
throughout the analysis:

```cpp
enum class JetVariation { Nominal, JESUp, JESDown, JERUp, JERDown };

const auto& ptFor(JetVariation v) {
  return v == JetVariation::Nominal ? Jet_pt_nom :
         v == JetVariation::JESUp   ? Jet_pt_jesTotalUp :
         v == JetVariation::JESDown ? Jet_pt_jesTotalDown :
         v == JetVariation::JERUp   ? Jet_pt_jerUp : Jet_pt_jerDown;
}
```

Run the full event selection and histogram filling once per requested
variation.  Use the matching MET variation (or the MET rebuilt in `nano_scan`)
in every observable involving MET; do not only vary the plotted jet pT.  Keep
JES and JER variations separate, and use the same nominal b-tag scale-factor
and weight conventions in every shape variation unless that systematic is also
being varied.

For reduced systematics, `jesTotal` is useful; for final measurements, preserve
the individual JES sources supplied by the payload and correlate a source only
where its payload definition says it is correlated.  Treat JER SF up/down as
shape variations, not event weights.

### Validation checklist

* Print the NanoAOD branch schema and fail clearly if the requested variation
  is absent.  Never silently fall back from a variation to nominal.
* Check that nominal re-corrected jets agree with the campaign's `Jet_pt`
  convention within the expected floating-point/payload precision.
* On MC, verify the response and resolution versus matched generator jets for
  nominal, JER-up, and JER-down; JER-up should broaden the response.
* Make a cutflow and key histograms for nominal, JES up/down, and JER up/down;
  jet threshold migrations and propagated MET changes must be visible.
* Unit-test deterministic random seeds and ensure data never enters the JER
  smearing branch.

The authoritative payload and matching prescription should always be taken from
the JERC release for the production era; payload names and NanoAOD branch names
change across campaigns.
