#include <vector>
#include <fstream>
#include <unordered_map>
#include <iostream>
#include <cmath>
#include <TTree.h>
#include <TFile.h>
#include <TPolyMarker3D.h>
#include <TCanvas.h>
#include <TVector3.h>
#include <TH2D.h>
#include <chrono>
#include "utils.h"
#include "structs.h"

bool near_top(double y, double eps = 0.1) {
  return std::abs(y - ytop) < eps;
}

bool near_mid(double y, double eps = 0.1) {
  return std::abs(y - ymid) < eps;
}

bool near_bot(double y, double eps = 0.1) {
  return std::abs(y - ybot) < eps;
}

/// coordinate where the three wires intersect
std::optional<TVector3> intersection(Wire plane2, Wire plane1, Wire plane0);

/// finds all intersections points of WireHits on one TPC wire plane
void wireHitIntersections(const std::vector<Wire> &wires,
                          const std::vector<WireHit> &tpcPlane,
                          std::vector<TVector3> &intersects);

#pragma clang diagnostic push
#pragma ide diagnostic ignored "Simplify" // IDE was angry that OUTPUT is const and wanted to simplify ifs with it

/// main method. n is the event to look at, or -1 to look at all events
/// makes some assumptions about directory structure: the dir this is run from has a `hitdumper_tree.root`,
/// and the dir above this has `WireDumpSBND.txt` and `StripDumpSBND.txt`
void wires(int n = -1, const char *hitdumperFile = "hitdumper_tree.root", bool thread = false, int bins = 10) {
  if (OUTPUT == Output::Draw && n == -1) {
    std::cout << "Cannot draw every event at once (ROOT gets angry)"
              << "\n Try calling wires with a specific (zero-indexed) event number"
              << "\n or compiling with OUTPUT = false"
              << std::endl;
    return;
  }
  // initialize ROOT stuff
  auto *c1 = new TCanvas("c1", "c1"); // to make root not print that this is created
  auto *file = TFile::Open(hitdumperFile);
  auto *hitdumper = (TDirectoryFile *) file->Get("hitdumper");
  hitdumper->cd();
  auto *tree = (TTree *) hitdumper->Get("hitdumpertree");

  // Histogram of % real hits
  auto *realHist = new TH1D("realHist", "percent real hits", bins, 0.0, 1.1);

  auto *nchitsHist = new TH1D("nchitsHist", "num chits", 10, 0.0, 1.0);
  auto *matchesHist = new TH1D("matchesHist", "num matches", 10, 0.0, 1.0);
//  matchesHist->SetStats(false);

  // load geometry
  std::vector<Wire> wires = parse_wires();
  std::vector<CRTStrip> strips = parse_strips();

  // Wire Hits
  int channel[WHITS], cryo[WHITS], tpc[WHITS], plane[WHITS], wire[WHITS];
  double peakT[WHITS];
  int nwhits;
  tree->SetBranchAddress("hit_channel", &channel);
  tree->SetBranchAddress("hit_cryostat", &cryo);
  tree->SetBranchAddress("hit_tpc", &tpc);
  tree->SetBranchAddress("hit_plane", &plane);
  tree->SetBranchAddress("hit_wire", &wire);
  tree->SetBranchAddress("hit_peakT", &peakT);
  tree->SetBranchAddress("nhits", &nwhits);

  // CRT Hits
  double x[CHITS], y[CHITS], z[CHITS], t[CHITS];
  int nchits;
  tree->SetBranchAddress("chit_x", &x);
  tree->SetBranchAddress("chit_y", &y);
  tree->SetBranchAddress("chit_z", &z);
  tree->SetBranchAddress("chit_time", &t);
  tree->SetBranchAddress("nchits", &nchits);

  using namespace std::chrono;

  std::chrono::microseconds makeTracksTime(0);
  std::chrono::microseconds sortTime(0);
  std::chrono::microseconds findIntersectsTime(0);
  std::chrono::microseconds scoreTime(0);
  std::chrono::microseconds dedupTime(0);
  std::chrono::microseconds totalTime(0);

  // draw crt hits
  auto *chitMarker = new TPolyMarker3D(CHITS);
  long low = (n == -1) ? 0 : n;
  long high = (n == -1) ? tree->GetEntries() : n + 1;
  for (long i = low; i < high; ++i) {
    int crtHitMatches = 0;

    tree->GetEntry(i);
    if (nchits > CHITS) {
      std::cout << "TOO FEW MAX CRT HITS, only " << CHITS << " of " << nchits << " hits are being used" << std::endl;
      nchits = CHITS;
    }
    if (nwhits > WHITS) {
      std::cout << "TOO FEW MAX WIRE HITS, only " << WHITS << " of " << nwhits << " hits are being used" << std::endl;
      nwhits = WHITS;
    }

    const auto start = high_resolution_clock::now();

    // read CRT/Wire hits from ROOT tree
    std::vector<CRTHit> chits;
    std::vector<WireHit> whits;
    for (int j = 0; j < nchits; ++j) {
      chits.emplace_back(x[j], y[j], z[j], t[j]);
      if constexpr (FLIP) {
        chitMarker->SetNextPoint(x[j], -z[j], y[j]);
      } else {
        chitMarker->SetNextPoint(x[j], y[j], z[j]);
      }
    }
    for (int j = 0; j < nwhits; ++j) {
      whits.emplace_back(channel[j], cryo[j], tpc[j], plane[j], wire[j], peakT[j]);
    }
//    std::cout << "nwhits = " << nwhits << std::endl;

    // top and mid are the planes above detector, bot is below
    std::vector<CRTHit> top, mid, bot;
    for (CRTHit hit : chits) {
      if (near_top(hit.y)) {
        top.push_back(hit);
      } else if (near_mid(hit.y)) {
        mid.push_back(hit);
      } else {
        bot.push_back(hit);
      }
    }

    // reconstruct CRTTracks (neither of the two in the ROOT tree worked for this, one because it was always empty)
    std::vector<CRTTrack> tracks;
    for (const auto &thit : top) {
      for (const auto &mhit : mid) {
        tracks.push_back(CRTTrack::topMid(thit.x, thit.y, thit.z, mhit.x, mhit.y, mhit.z));
      }
    }
    // the bottom plane doesn't have x coord because of it only has one plane of strips, so the actual
    // x coordinate is projected down from the top planes. Replaces all top->mid tracks with (potentially multiple)
    // top->mid->bot tracks.
    auto *projPts = new TPolyMarker3D();
    if (!bot.empty()) {
      for (size_t j = tracks.size(); j > 0; --j) {
        auto track = tracks[0];
        tracks.erase(tracks.begin());
        for (const auto &bhit : bot) {
          TVector3 proj = track.projectY(bhit.y);
          TVector3 bvec(bhit.x, bhit.y, bhit.z);
          double dist = (bvec - proj).Mag();
          bool sameSide = std::signbit(proj.x()) == std::signbit(bhit.x);
          if (sameSide && dist < 600) { // TODO tune this a bit (maybe instead check that proj is inside CRT?)
            addPoint(projPts, TVector3(proj.x(), bhit.y, bhit.z));
            tracks.emplace_back(track, proj.x(), bhit.y, bhit.z);
          }
        }
      }
    }
    if constexpr (OUTPUT == Output::Draw) {
      projPts->SetMarkerStyle(kFullDotMedium);
      projPts->SetMarkerColor(kGreen);
      projPts->Draw();
    }

    unsigned long firstIncomplete = tracks.size();
    // every permutation of incomplete top->bot and mid->bot tracks
    for (const auto &bhit : bot) {
      for (const auto &thit : top) {
        tracks.push_back(CRTTrack::topBot(thit.x, thit.y, thit.z, bhit.x, bhit.y, bhit.z));
      }
      for (const auto &mhit : mid) {
        tracks.push_back(CRTTrack::midBot(mhit.x, mhit.y, mhit.z, bhit.x, bhit.y, bhit.z));
      }
    }

    // remove duplicate top->bot or mid->bot tracks that are already part of a top->mid->bot track
    if (firstIncomplete != 0) {
      for (unsigned long j = tracks.size() - 1; j >= firstIncomplete; --j) {
        auto incomp = tracks[j];
        for (unsigned long k = 0; k < firstIncomplete; ++k) {
          auto comp = tracks[k];
          if (incomp.botEq(comp) && (incomp.topEq(comp) || incomp.midEq(comp))) {
            tracks.erase(tracks.begin() + j);
            break;
          }
        }
      }
    }
//    std::cout << "#tracks = " << tracks.size() << std::endl;

    const auto madeTracks = high_resolution_clock::now();
    makeTracksTime += duration_cast<microseconds>(madeTracks - start);

    // Sort by time then by plane (vertical plane first)
    std::sort(whits.begin(), whits.end(), [](const WireHit &a, const WireHit &b) {
      if (a.peakTick < b.peakTick) {
        return true;
      } else if (a.peakTick == b.peakTick) {
        return a.plane > b.plane;
      } else return false;
    });

    std::vector<WireHit> tpc0, tpc1;
    for (WireHit whit : whits) {
      if (whit.tpc == 0) {
        tpc0.push_back(whit);
      } else {
        tpc1.push_back(whit);
      }
    }

    const auto sorted = high_resolution_clock::now();
    sortTime += duration_cast<microseconds>(sorted - madeTracks);

    std::vector<TVector3> intersects0;
    wireHitIntersections(wires, tpc0, intersects0);
    std::vector<TVector3> intersects1;
    wireHitIntersections(wires, tpc1, intersects1);
//    std::cout << "num intersects = " << intersects.size() << std::endl;
    if constexpr (OUTPUT == Output::Draw) {
      auto *intersectMarks = new TPolyMarker3D();
      for (const auto &intersect : intersects0) {
        addPoint(intersectMarks, intersect);
      }
      for (const auto &intersect : intersects1) {
        addPoint(intersectMarks, intersect);
      }
      intersectMarks->SetMarkerStyle(kFullDotMedium);
      intersectMarks->SetMarkerColor(kRed);
      intersectMarks->Draw();
    }

    const IntersectsByHeight<NHeightBins> byHeight0(intersects0);
    const IntersectsByHeight<NHeightBins> byHeight1(intersects1);

    const auto foundIntersects = high_resolution_clock::now();
    findIntersectsTime += duration_cast<microseconds>(foundIntersects - sorted);

    // score each track: walk down track, project it onto the wire plane, and count nearby wire hits
    hashmap<CRTTrack, double> scores;
    for (const auto &track : tracks) {
      double score = 0;
      int tot = 0;

      auto topmost = track.topmostPt();
      double slopeZ = (topmost.z() - track.zb) / (topmost.y() - track.yb);
      double slopeX = (topmost.x() - track.xb) / (topmost.y() - track.yb);
      int delta = 5;
      for (
          TVector3 pt = track.topmostPt();
          pt.Y() > -200; // -200 is the bottom of tcp plane
          pt -= TVector3(slopeX * delta, delta, slopeZ * delta)
          ) {
        if (pt.Y() > 200) continue; // 200 is top of tcp plane
//        const std::vector<TVector3> &intersects = std::signbit(pt.x())
//                                 ? intersects0
//                                 : intersects1;
        const IntersectsByHeight<NHeightBins> &byHeight = std::signbit(pt.x())
                                                          ? byHeight0
                                                          : byHeight1;
        const auto[at, near] = byHeight.at_y(pt.y());
        for (const auto &intersect : at.get()) {
          ++tot;
          auto projected = pt;
          projected.SetX(intersect.x());
          double mag = (projected - intersect).Mag();
          int maxDist = 5;
          if (mag < maxDist) {
            ++score;
          }
        }
        if (near) {
          for (const auto &intersect : near->get()) {
            ++tot;
            auto projected = pt;
            projected.SetX(intersect.x());
            double mag = (projected - intersect).Mag();
            int maxDist = 5;
            if (mag < maxDist) {
              ++score;
            }
          }
        }
      }

      if (tot == 0) {
        score = 0;
      } else {
        score /= tot;
      }
//      std::cout << "score = " << score << "\t\t\t" << track << std::endl;
      scores.emplace(track, score);
    }

//    std::cout << intersects0.size() + intersects1.size() << " tot intersects" << std::endl;

    const auto scored = high_resolution_clock::now();
    scoreTime += duration_cast<microseconds>(scored - foundIntersects);
    if (duration_cast<microseconds>(scored - foundIntersects) > milliseconds(500)) {
      std::cout << "Took " << duration_cast<microseconds>(scored - foundIntersects).count() << " us" << std::endl;
      std::cout << chits.size() << " crt hits\n"
                << whits.size() << " wire hits\n"
                << intersects0.size() + intersects1.size() << " tot intersects"
                << std::endl;
    }

    // each track to a list of of top/bot/mid points on that track
    hashmap<CRTTrack, std::vector<TVector3>> trackToHits;
    // each point to a list of each track that includes that point
    hashmap<TVector3, std::vector<CRTTrack>> hitToTracks;
    for (const CRTTrack track : tracks) {
      trackToHits.emplace(track, std::vector<TVector3>());
      std::optional<TVector3> top = track.top();
      if (top) {
        trackToHits[track].push_back(*top);
        if (hitToTracks.find(*top) == hitToTracks.end()) { // not in map
          hitToTracks.insert({*top, std::vector<CRTTrack>()});
        }
        hitToTracks[*top].push_back(track);
      }
      std::optional<TVector3> mid = track.mid();
      if (mid) {
        trackToHits[track].push_back(*mid);
        if (hitToTracks.find(*mid) == hitToTracks.end()) { // not in map
          hitToTracks.insert({*mid, std::vector<CRTTrack>()});
        }
        hitToTracks[*mid].push_back(track);
      }
      std::optional<TVector3> bot = track.bot();
      if (bot) {
        // todo what's the default ± value for x?
        TVector3 xFixedBot = TVector3(0.0, bot->y(), bot->z());
        trackToHits[track].push_back(xFixedBot);
        if (hitToTracks.find(xFixedBot) == hitToTracks.end()) { // not in map
          hitToTracks.insert({xFixedBot, std::vector<CRTTrack>()});
        }
        hitToTracks[xFixedBot].push_back(track);
      }
    }

    // only the tracks that match a wire intersects "track"
    std::vector<CRTTrack> matches;
    // hacky solution until I think of a better way to get at least one per repeated x
    hashset<double> usedScores;
    for (const CRTTrack &track : tracks) {
      std::vector<TVector3> hits = trackToHits[track];
      bool best = true;
      if (scores[track] > 0.000001) {
        for (const TVector3 &hit : hits) {
          std::vector<CRTTrack> htracks = hitToTracks[hit];
          for (const CRTTrack &htrack : htracks) {
            if (track == htrack) continue;
            if (scores[track] < scores[htrack]) {
              best = false;
              break;
            } else if (scores[track] == scores[htrack]) {
              if (usedScores.find(scores[track]) != usedScores.end()) { // score already used
                best = false;
                break;
              }
            }
          }
        }
      } else {
        best = false;
      }
      if (best) {
        matches.push_back(track);
        usedScores.insert(scores[track]);
      }
    }

    const auto deduped = high_resolution_clock::now();
    dedupTime += duration_cast<microseconds>(deduped - scored);
    totalTime += duration_cast<microseconds>(deduped - start);

//    std::cout << "matches.size() = " << matches.size() << std::endl;
    for (const auto &match : matches) {
      match.draw();
    }

    int nmatch = 0;
    for (const auto &chit : chits) {
      for (const auto &match : matches) {
        if (match.containsPt(chit)) {
          ++nmatch;
        }
      }
    }

    double realPct = ((double) nmatch) / nchits;
    std::cout << "realPct = " << realPct << std::endl;
    realHist->Fill(realPct);
    nchitsHist->Fill(nchits);
    matchesHist->Fill(nmatch);

//    std::cout << std::endl;
  } // end of for loop over all events

  std::cout << "totalTime = " << totalTime.count() << std::endl;
  std::cout << "makeTracksTime = " << makeTracksTime.count() << std::endl;
  std::cout << "sortTime = " << sortTime.count() << std::endl;
  std::cout << "findIntersectsTime = " << findIntersectsTime.count() << std::endl;
  std::cout << "scoreTime = " << scoreTime.count() << std::endl;
  std::cout << "dedupTime = " << dedupTime.count() << std::endl;

  if constexpr (OUTPUT == Output::Draw) {
    chitMarker->SetMarkerStyle(kFullDotMedium);
    chitMarker->SetMarkerColor(kBlue);
    chitMarker->Draw();
  }

  if constexpr (OUTPUT == Output::Histogram) {
    realHist->Draw();
//    nchitsHist->Draw();
//    matchesHist->Draw();
  }

//   draw wires
  for (size_t i = 0; i < wires.size(); i += 50) {
    wires[i].draw();
  }

  // draw strips
/*
  for (int i = 0; i < strips.size(); i += 3) {
    strips[i].draw();
  }
*/
}

#pragma clang diagnostic pop

void wires(const char *hitdumperFile, bool thread = false, int n = -1) {
  wires(n, hitdumperFile, thread);
}

/// coordinate where the three wires intersect
void wireHitIntersections(
    const std::vector<Wire> &wires,
    const std::vector<WireHit> &tpcPlane,
    std::vector<TVector3> &intersects
) {
  std::vector<WireHit> plane2, plane1, plane0;
  for (WireHit whit : tpcPlane) {
    if (whit.plane == 2) {
      plane2.push_back(whit);
    } else if (whit.plane == 1) {
      plane1.push_back(whit);
    } else {
      plane0.push_back(whit);
    }
  }

  for (const WireHit &p2 : plane2) {
    double t2 = p2.peakTick;
    for (const WireHit &p1 : plane1) {
      double t1 = p1.peakTick;
      double diff = t2 - t1;
      if (diff < 3 || diff > 4) continue;
      for (const WireHit &p0 : plane0) {
        double t0 = p0.peakTick;
        double diff = t1 - t0;
        if (diff < 3 || diff > 4) continue;
        auto intersect = intersection(wires[p2.channel], wires[p1.channel], wires[p0.channel]);
        if (intersect) {
          intersects.push_back(*intersect);
        }
      }
    }
  }
}

/// finds all intersections points of WireHits on one TPC wire plane
std::optional<TVector3> intersection(Wire plane2, Wire plane1, Wire plane0) {
  if (plane2.plane != 2 || plane1.plane != 1 || plane0.plane != 0) {
    return std::optional<TVector3>();
  }

  double x = plane0.x1; // entire tpc-plane is same x

  // slope: always 60 degrees, have to pick + or -
  double m1 = 1.0 / sqrt(3.0);
  auto[p0y1, p0y2] = (plane0.z1 < plane0.z2)
                     ? std::make_pair(plane0.y1, plane0.y2)
                     : std::make_pair(plane0.y2, plane0.y1);
  if (p0y2 > p0y1) {
    m1 *= -1;
  }

  // intersection of wire 1 and wire 0
  double z = (m1 * (plane1.z1 + plane0.z1) + plane0.y1 - plane1.y1) / (2 * m1);

  double y = m1 * (z - plane1.z1) + plane1.y1;

  return std::optional<TVector3>(TVector3(x, y, plane2.z1));
//  return std::optional<TVector3>(TVector3(x, y, z));
}