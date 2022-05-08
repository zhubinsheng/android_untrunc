/*
	Untrunc - track.h

	Untrunc is GPL software; you can freely distribute,
	redistribute, modify & use under the terms of the GNU General
	Public License; either version 2 or its successor.

	Untrunc is distributed under the GPL "AS IS", without
	any warranty; without the implied warranty of merchantability
	or fitness for either an expressed or implied particular purpose.

	Please see the included GNU General Public License (GPL) for
	your rights and further details; see the file COPYING. If you
	cannot, write to the Free Software Foundation, 59 Temple Place
	Suite 330, Boston, MA 02111-1307, USA.  Or www.fsf.org

	Copyright 2010 Federico Ponchio

														*/

#include <assert.h>
#include <string>
#include <iostream>
#include <iomanip>  // setprecision
#include <algorithm>
#include <fstream>

extern "C" {
#include <stdint.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

#include "mp4.h"
#include "atom.h"
#include "file.h"

using namespace std;

uint64_t Mp4::step_ = 1;

Mp4::~Mp4() {
	delete root_atom_;
}

void Mp4::parseHealthy() {
	header_atom_ = root_atom_->atomByNameSafe("mvhd");
	readHeaderAtom();

	// https://github.com/FFmpeg/FFmpeg/blob/70d25268c21cbee5f08304da95be1f647c630c15/doc/APIchanges#L86
    #if ( LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100) )
	av_register_all();
    #endif

	unmute(); // sets AV_LOG_LEVEL
	context_ = avformat_alloc_context();
	// Open video file
	int error = avformat_open_input(&context_, filename_ok_.c_str(), NULL, NULL);

	if(error != 0)
		throw "Could not parse AV file (" + to_string(error) + "): " + filename_ok_;

	if(avformat_find_stream_info(context_, NULL) < 0)
		throw "Could not find stream info";

	av_dump_format(context_, 0, filename_ok_.c_str(), 0);

	parseTracksOk();

//	if (g_show_tracks) return;  // show original track order

	// reduce false positives when matching
	map<string, int> certainty = {
	    {"gpmd", 4},
	    {"fdsc", 3},
	    {"mp4a", 2},
	    {"avc1", 1},
	    // default is 0
	};
	sort(tracks_.begin(), tracks_.end(),  [&](const Track& a, const Track& b) -> bool {
		return certainty[a.codec_.name_] > certainty[b.codec_.name_];
	});

	if (hasCodec("fdsc") && hasCodec("avc1"))
		getTrack("avc1").codec_.strictness_lvl_ = 1;

	for (uint i=0; i < tracks_.size(); i++)
		if (contains({"twos", "sowt"}, tracks_[i].codec_.name_)) twos_track_idx_ = i;
	if (twos_track_idx_ >= 0 && hasCodec("avc1")) {
		getTrack("avc1").codec_.chk_for_twos_ = true;
	}

	if (g_log_mode >= LogMode::I) cout << '\n';
}


void Mp4::parseOk(const string& filename, bool accept_unhealthy) {
	filename_ok_ = filename;
	auto& file = openFile(filename);

	logg(I, "parsing healthy moov atom ... \n");
	root_atom_ = new Atom;
	while (true) {
		Atom *atom = new Atom;
		try {
			atom->parse(file);
		} catch (string e) {
			logg(W, "failed decoding atom: ", e, "\n");
			break;
		}
		root_atom_->children_.push_back(atom);
		if(file.atEnd()) break;
	}

	if(root_atom_->atomByName("ctts"))
		cerr << "Composition time offset atom found. Out of order samples possible." << endl;

	if(root_atom_->atomByName("sdtp"))
		cerr << "Sample dependency flag atom found. I and P frames might need to recover that info." << endl;

	auto ftyp = root_atom_->atomByName("ftyp", true);
	if (ftyp) {
		ftyp_ = ftyp->getString(0, 4);
		logg(V, "ftyp_ = '", ftyp_, "'\n");
	}
	else {
		logg(V, "no 'ftyp' atom found\n");
	}

	if (ftyp_ == "XAVC") {
		logg(V, "detected 'XAVC', deactivating 'g_strict_nal_frame_check'\n");
		g_strict_nal_frame_check = false;
		g_ignore_forbidden_nal_bit = false;
	}

	has_moov_ = root_atom_->atomByName("moov", true);

	if (has_moov_)
		parseHealthy();
	else
		if (accept_unhealthy) logg(W, "no 'moov' atom found\n");
		else logg(ET, "no 'moov' atom found\n");
}

void Mp4::parseTracksOk() {
	Codec::initOnce();
	auto mdats = root_atom_->atomsByName("mdat", true);
	if (mdats.size() > 1)
		logg(W, "multiple mdats detected, see '-ia'\n");

	orig_mdat_start_ = mdats.front()->start_;

	auto traks = root_atom_->atomsByName("trak");
	for (uint i=0; i < traks.size(); i++) {
		tracks_.emplace_back(traks[i], context_->streams[i]->codecpar, timescale_);
		auto& track = tracks_.back();
		track.parseOk();

		assert(track.chunks_.size());
		if (!g_ignore_out_of_bound_chunks) {
			assert(track.chunks_.front().off_ >= mdats.front()->contentStart());
			assert(track.chunks_.back().off_ < mdats.back()->start_ + mdats.back()->length_);
		}

		max_part_size_ = max(max_part_size_, track.max_allowed_ss_);
	}

	if (g_max_partsize > 0) {
		logg(V, "ss: using manually specified: ", g_max_partsize, "\n");
		max_part_size_ = g_max_partsize;
	}
}


void Mp4::printMediaInfo() {
	if (has_moov_) {
		printTracks();
		cout << "\n\n";
		printAtoms();
		cout << "\n\n";
		printDynStats();
	}
	else {
		printAtoms();
	}
}

void Mp4::printTracks() {
	cout << "tracks:\n";
	for (uint i=0; i < tracks_.size(); i++) {
		auto& track = tracks_[i];
		cout << "  [" << i << "] " << track.handler_type_ << " by '" << track.handler_name_ << "' ";
		if (track.codec_.name_.size())
			cout << "(" << track.codec_.name_ << ") ";
		auto codec_type = av_get_media_type_string(track.codec_.av_codec_params_->codec_type);
		auto codec_name = avcodec_get_name(track.codec_.av_codec_params_->codec_id);
		cout << ss("<", codec_type, ", ", codec_name, ">\n");
	}

}

void Mp4::printAtoms() {
	if (!root_atom_) return;
	for (auto& c : root_atom_->children_)
		c->print(0);
}

void Mp4::printDynStats() {
	if (first_off_abs_ < 0) genDynStats(true);
	cout << "\ndynamic stats:";
	cout << "first_off_: " << first_off_abs_ << '\n';
	cout << "first_off_rel_: " << first_off_rel_ << '\n';
	cout << "max_part_size_: " << max_part_size_ << '\n';
	for (auto& t : tracks_)
		t.printDynStats();
}

void Mp4::listm(const string& filename) {
	struct MoovStats {
		off_t atom_start_;
		off_t min_off_ = numeric_limits<off_t>::max(), max_off_ = -1;
		int n_tracks = 0;
		MoovStats(off_t start) : atom_start_(start) {}
		explicit operator bool() { return n_tracks > 0; }

	};

	FileRead f(filename);
	map<string, int> cnt;
	vector<Atom*> moovs, mdats;

	auto printMoov = [](Atom& moov) {
		static int idx = 0;
		vector<string> vc;
		auto ms = MoovStats(moov.start_);

		for (Atom* trak : moov.atomsByName("trak")) {
			Track t (trak, NULL, 0);
			vc.emplace_back(t.getCodecNameSlow());
			t.getChunkOffsets();
			ms.min_off_ = min(ms.min_off_, t.chunks_.front().off_);
			ms.max_off_ = max(ms.max_off_, t.chunks_.back().off_);
			ms.n_tracks++;
		}
		cout << ss(idx++, ": ", setw(12), ms.min_off_, " : ", setw(12), ms.max_off_,
		       ", width=", setw(12), ms.max_off_ - ms.min_off_, ", ", setw(28), left, vecToStr(vc), right,
		       " (start=", setw(13), ss(ms.atom_start_, ","), " moov_sz=", setw(12), moov.length_, ")\n");
	};

	auto printMdat = [](Atom& atom) {
		static int idx = 0;
		cout << ss(idx++, ": ", setw(85), " (start=", setw(13), ss(atom.start_, ","), " mdat_sz=", setw(12), atom.length_, ")\n");
	};

	for (Atom& atom : AllAtomsIn(f)) {
		if (atom.name_ == "moov") {
			f.seek(atom.start_);
			moovs.emplace_back(new Atom(f));
		} else if (atom.name_ == "mdat") {
			f.seek(atom.start_);
			mdats.emplace_back(new Atom(f));
		}
	}

	cout << "moovs:\n";
	for (Atom* atom : moovs) {printMoov(*atom); delete atom;}
	cout << "mdats:\n";
	for (Atom* atom : mdats) {printMdat(*atom); delete atom;}
}

void Mp4::unite(const string& mdat_fn, const string& moov_fn) {
	string output = mdat_fn + "_united.mp4";
	warnIfAlreadyExists(output);

	FileRead fmdat(mdat_fn), fmoov(moov_fn);

	BufferedAtom mdat(fmdat), moov(fmoov);
	if (g_range_start != kRangeUnset) mdatFromRange(fmdat, mdat);
	else assert(findAtom(fmdat, "mdat", mdat));
	assert(findAtom(fmoov, "moov", moov));

	bool force_64 = mdat.header_length_ > 8;
	logg(V, "force_64: ", force_64, '\n');

	FileWrite fout(output);
	if (g_range_start != kRangeUnset) {
		Atom free;
		free.name_ = "free";
		free.start_ = - 8;
		free.content_.resize(mdat.start_ - 8);
		free.updateLength();
		free.write(fout);
	}
	else {
		fout.copyRange(fmdat, 0, mdat.start_);
	}
	mdat.updateFileEnd(fmdat.length());
	moov.file_end_ = moov.start_ + moov.length_;  // otherwise uninitialized
	mdat.write(fout, force_64);
	moov.write(fout);
}

void Mp4::shorten(const string& filename, int mega_bytes, bool force) {
	int64_t n_bytes = mega_bytes * 1e6;
	string suf = force ? "_fshort-" : "_short-";
	string output = ss(filename + suf, mega_bytes, getMovExtension(filename));
	warnIfAlreadyExists(output);

	FileRead f(filename);
	BufferedAtom mdat(f), moov(f);
	if (f.length() <= n_bytes)
		logg(ET, "file too small\n");

	bool good_structure = isPointingAtAtom(f);
	if (good_structure) assert(findAtom(f, "mdat", mdat));

	FileWrite fout(output);
	if (!good_structure || !findAtom(f, "moov", moov) || moov.start_ < mdat.start_) {
		fout.copyN(f, 0, n_bytes);
	}
	else {
		auto mdat_end_real = mdat.start_ + mdat.length_;
		off_t n_after_mdat = f.length() - mdat_end_real;

		if (n_bytes < n_after_mdat) {
			if (!force) logg(ET, "moov might not be fully contained (", n_after_mdat, " < ", n_bytes, "), force with '-fsh'\n");
			fout.copyN(f, 0, n_bytes);
			return;
		}

		fout.copyRange(f, 0, mdat.start_);
		mdat.updateFileEnd(n_bytes - n_after_mdat);
		mdat.write(fout, mdat.header_length_ > 8);
		fout.copyN(f, mdat_end_real, n_after_mdat);
	}
}

void Mp4::makeStreamable(const string& ok, const string& output) {
	warnIfAlreadyExists(output);
	parseOk(ok);
	if (!current_mdat_) findMdat(*current_file_);

	auto moov = root_atom_->atomByName("moov");
	auto mdat = root_atom_->atomByName("mdat");
	if (moov->start_ < mdat->start_) {
		logg(I, "already streamable!\n");
		return;
	}

	for (auto& t : tracks_) {
		pkt_idx_ += t.sizes_.size();
		for (auto& c : t.chunks_) c.off_ -= mdat->contentStart();
	}

	saveVideo(output);
}

void Mp4::analyzeOffset(const string& filename, off_t real_offset) {
	FileRead file(filename);
	auto mdat = findMdat(file);
	if (real_offset < mdat->contentStart() || real_offset >= mdat->file_end_)
		throw "given offset is not in 'mdat'";

	auto buff = file.getPtrAt(real_offset, 16);
	printBuffer(buff, 16);

	auto off = real_offset - mdat->contentStart();
	auto match = getMatch(off);
	dumpMatch(match, 0);
}

void Mp4::chkStrechFactor() {
	int video = 0, sound = 0;
	for(Track& track : tracks_) {
		int msec = track.getDurationInMs();
		if (track.handler_type_ == "vide") video = msec;
		else if (track.handler_type_ == "soun") sound = msec;
	}
	if (!sound) return;

	double factor = (double) sound / video;
	double eps = 0.1;
	if (fabs(factor - 1) > eps) {
		if (!g_stretch_video)
			cout << "Tip: Audio and video seem to have different durations (" << factor << ").\n"
			     << "     If audio and video are not in sync, give `-sv` a try. See `--help`\n";
		else
			for(Track& track : tracks_) {
				if (track.handler_type_ == "vide") {
					track.stretch_factor_ = (double)sound / video;
//					track.stretch_factor_ = 0.5;
					logg(I, "changing video speed by factor " , 1/factor, "\n");
					track.duration_ *= factor;
					break;
				}
			}
	}

}

void Mp4::setDuration() {
	for(Track& track : tracks_) {
		if (contains(ignore_duration_, track.codec_.name_)) continue;
		duration_ = max(duration_, track.getDurationInTimescale());
	}
}

void Mp4::saveVideo(const string& filename) {
	/* we save all atom except:
	  cslg: would need to be recalculated (from ctts)
	  stps: partial sync, same as sync

	  movie is made by ftyp, moov, mdat (we need to know mdat begin, for absolute offsets)
	  assumes offsets in stco are absolute and so to find the relative just subtrack mdat->start + 8
*/

	if (tracks_.back().is_dummy_) tracks_.pop_back();

	if (g_log_mode >= I) {
		cout << "Info: Found " << pkt_idx_ << " packets ( ";
		for(const Track& t : tracks_){
			cout << t.codec_.name_ << ": " << t.getNumSamples() << ' ';
			if (contains({"avc1", "hvc1"}, t.codec_.name_))
				cout << ss(t.codec_.name_, "-keyframes: ", t.keyframes_.size(), " ");
		}
		cout << ")\n";
	}

	chkStrechFactor();
	setDuration();

	for(Track& track : tracks_) {
		track.applyExcludedToOffs();
		track.writeToAtoms(broken_is_64_);

		auto& cn = track.codec_.name_;
		if (contains(ignore_duration_, cn)) continue;

		int hour, min, sec, msec;
		int bmsec = track.getDurationInMs();

		auto x = div(bmsec, 1000); msec = x.rem; sec = x.quot;
		x = div(sec, 60); sec = x.rem; min = x.quot;
		x = div(min, 60); min = x.rem; hour = x.quot;
		string s_msec = (msec?to_string(msec)+"ms ":"");
		string s_sec = (sec?to_string(sec)+"s ":"");
		string s_min = (min?to_string(min)+"min ":"");
		string s_hour = (hour?to_string(hour)+"h ":"");
		logg(I, "Duration of ", cn, ": ", s_hour, s_min, s_sec, s_msec, " (", bmsec, " ms)\n");

	}

	auto& mdat = current_mdat_;
	Atom* original_mdat = root_atom_->atomByName("mdat");
	root_atom_->replace(original_mdat, mdat);
	delete original_mdat;
//	Atom *mdat = root_atom_->atomByName("mdat");

	if (unknown_lengths_.size()) {
		cout << setprecision(4);
		int64_t bytes_not_matched = 0;
		for (auto n : unknown_lengths_) bytes_not_matched += n;
		double percentage = (double)100 * bytes_not_matched / mdat->contentSize();
		logg(W, "Unknown sequences: ", unknown_lengths_.size(), '\n');
		logg(W, "Bytes NOT matched: ", pretty_bytes(bytes_not_matched), " (", percentage, "%)\n");
	}

	if (g_dont_write) return;

	editHeaderAtom();

	Atom *ftyp = root_atom_->atomByName("ftyp");
	Atom *moov = root_atom_->atomByName("moov");

//	moov->prune("ctts");
	moov->prune("cslg");
	moov->prune("stps");

	root_atom_->updateLength();

	// remove empty tracks
	for (auto it=tracks_.begin(); it != tracks_.end();) {
		if (!it->getNumSamples()) {
			moov->prune(it->trak_);
			logg(I, "pruned empty '", it->codec_.name_, "' track\n");
			it = tracks_.erase(it);
		}
		else it++;
	}

	//fix offsets
	off_t offset = mdat->newHeaderSize() + moov->length_;
	if(ftyp)
		offset += ftyp->length_; //not all mov have a ftyp.

	for (Track& track : tracks_) {
		assert(track.chunks_.size());
		for (auto& c : track.chunks_) c.off_ += offset;
		track.saveChunkOffsets(); //need to save the offsets back to the atoms
	}

	if (g_dump_repaired) {
		auto dst = filename + ".dump";
		cout << '\n';
		cout << "n_to_dump: " << to_dump_.size() << (to_dump_.size() ? "\n" : " (dumping all)\n");
		logg(I, "dumping to: '", dst, "'\n");
		ofstream f_out(dst);
		cout.rdbuf(f_out.rdbuf());

		filename_ok_ = filename;
		mdat->start_ = offset - 8;  // not mdat->headerSize()
		current_mdat_ = (BufferedAtom*) mdat;
		dumpSamples();
		exit(0);
	}

	//save to file
	logg(I, "saving ", filename, '\n');
	FileWrite file(filename);

	if(ftyp)
		ftyp->write(file);
	moov->write(file);
	mdat->write(file);
}

string Mp4::getOutputSuffix() {
	string output_suffix;
	if (g_ignore_unknown) output_suffix += ss("-s", Mp4::step_);
	if (g_use_chunk_stats) output_suffix += "-dyn";
	if (g_dont_exclude) output_suffix += "-k";
	if (g_stretch_video) output_suffix += "-sv";
	return output_suffix;
}

void Mp4::chkUntrunc(FrameInfo& fi, Codec& c, int i) {
	auto offset = fi.offset_;
	auto start = loadFragment(offset);

	cout << "\n(" << i << ") Size: " << fi.length_ << " offset: " << offToStr(offset)
	     << "  begin: " << mkHexStr(start, 4) << " " << mkHexStr(start+4, 4);
	auto end_off = offset + fi.length_ - 4;
	auto sz = min(to_int64(8), current_mdat_->contentSize() - end_off);
	auto end = current_mdat_->getFragment(end_off, sz);
	cout << " end: " << mkHexStr(end, sz) << '\n';
	start = loadFragment(offset);

	bool ok = true;
	bool matches = false;
	for (auto& t : tracks_)
		if (t.codec_.matchSample(start)){
			if (t.codec_.name_ != c.name_){
				cout << "Matched wrong codec! '" << t.codec_.name_ << "' instead of '" << c.name_ << "'\n";
				ok = false;
			} else {matches = true; break;}
		}
	uint size = c.getSize(start, current_maxlength_, offset);
	uint duration = c.audio_duration_;
	//TODO check if duration is working with the stts duration.

	if(!matches) {
		cout << "Match failed! '" << c.name_ << "' itself not detected\n";
		ok = false;
	}

	cout << "detected size: " << size << " true: " << fi.length_;
	if (size != fi.length_) cout << "  <- WRONG";
	cout << '\n';

	if (c.name_ == "mp4a") {
		cout << "detected duration: " << duration << " true: " << fi.audio_duration_;
		if (duration != fi.audio_duration_) cout << "  <- WRONG";
		cout << '\n';
		if (c.was_bad_) cout << "detected bad frame\n";
	}

	if (fi.keyframe_){
		cout << "detected keyframe: " << c.was_keyframe_  << " true: 1\n";
		if (!c.was_keyframe_){
			cout << "keyframe not detected!";
			ok = false;
		}
	}

	if (!ok) hitEnterToContinue();
	//assert(length == track.sizes[i]);

}

void Mp4::analyze(bool gen_off_map) {
	auto& mdat = current_mdat_;
	if (!mdat) findMdat(*current_file_);
	assert(mdat);

	for (uint idx=0; idx < tracks_.size(); idx++) {
		Track& track = tracks_[idx];
		if (!gen_off_map) cout << "\nTrack " << idx << " codec: " << track.codec_.name_ << endl;

		if (track.isChunkTrack()) {
			track.genChunkSizes();
			for (uint i=0; i < track.chunks_.size(); i++) {
				auto& c = track.chunks_[i];
				auto off = c.off_ - mdat->contentStart();
				assert(c.size_ % c.n_samples_ == 0);
				off_to_chunk_[off] = Mp4::Chunk(off, c.n_samples_, idx, c.size_ / c.n_samples_);
			}
		}
		else {
			uint k = track.keyframes_.size() ? track.keyframes_[0] : -1, ik = 0;

			auto c_it = track.chunks_.begin();
			size_t sample_idx_in_chunk = 0;
			off_t cur_off = c_it->off_ - mdat->contentStart();
			for (uint i=0; i < track.sizes_.size(); i++) {
				bool is_keyframe = k == i;

				if (is_keyframe && ++ik < track.keyframes_.size())
					k = track.keyframes_[ik];

				auto sz = track.getSize(i);
				auto fi = FrameInfo(idx, is_keyframe, track.getTime(i), cur_off, sz);

				if (gen_off_map) off_to_frame_[cur_off] = fi;
				else {
					if (cur_off > mdat->contentSize()) {
						logg(W, "reached premature end of mdat\n");
						break;
					}
					chkUntrunc(fi, track.codec_, i);
				}

				if (++sample_idx_in_chunk >= to_uint(c_it->n_samples_)) {
					cur_off = (++c_it)->off_ - mdat->contentStart();
					sample_idx_in_chunk = 0;
				}
				else
					cur_off += sz;
			}
		}
	}
}

void Mp4::genChunks() {
	for (auto& t : tracks_) t.genChunkSizes();
}

void Mp4::genChunkTransitions() {
	ChunkIt::Chunk last_chunk;

	tracks_.emplace_back("free");
	idx_free_ = tracks_.size()-1;
	logg(V, "created dummy track 'free'\n");

	for (auto& cur_chunk : AllChunksIn(this, false)) {
		auto track_idx = cur_chunk.track_idx_;
		auto off = cur_chunk.off_;

		if (cur_chunk.should_ignore_) goto loop_end;

		if (first_off_abs_ < 0) {
			first_off_abs_ = cur_chunk.off_;
			first_off_rel_ = first_off_abs_ - current_mdat_->contentStart();
			orig_first_track_ = &tracks_[track_idx];
		}

		if (last_chunk) {
			auto last_end = last_chunk.off_ + last_chunk.size_;
			if (off - current_mdat_->contentStart() < pat_size_ / 2) {
				logg(W, "rel_off(cur_chunk) < pat_size/2 .. ", tracks_[track_idx].codec_.name_, " ", cur_chunk, "\n");
				goto loop_end;
			}
			assert(off >= last_end);
			if (off != last_end) {
				chunk_transitions_[{last_chunk.track_idx_, idx_free_}].emplace_back(last_end);
				tracks_[idx_free_].chunks_.emplace_back(last_end, off - last_end, off - last_end);
				chunk_transitions_[{idx_free_, track_idx}].emplace_back(off);
			}
			else {
				chunk_transitions_[{last_chunk.track_idx_, track_idx}].emplace_back(off);
			}
		}

        loop_end:
		last_chunk = cur_chunk;
	}

	if (!tracks_.back().chunks_.size()) {
		tracks_.pop_back();
		idx_free_ = kDefaultFreeIdx;
		logg(V, "removed dummy track 'free'\n");
	}
}

void Mp4::genTrackOrder() {
	if (!current_mdat_) findMdat(*current_file_);

	vector<pair<int, int>> order;
	for (auto& cur_chunk : AllChunksIn(this, true)) {
		auto track_idx = cur_chunk.track_idx_;
		if (order.size() > 100) break;
		order.emplace_back(make_pair(track_idx, cur_chunk.n_samples_));
	}

	track_order_simple_ = findOrderSimple(order);

	if (g_log_mode >= LogMode::V) {
		logg("order ( ", order.size(), "): ");
		for (auto& p : order)
			logg("(", p.first, ", ", p.second, ") ");
		logg("\n");
	}
	if (findOrder(order))
		track_order_ = order;
}

buffs_t Mp4::offsToBuffs(const offs_t& offs, const string& load_prefix) {
	int cnt=0;
	buffs_t buffs;
	for (auto off : offs) {
		if (g_log_mode == I) outProgress(cnt++, offs.size(), load_prefix);
		auto buff = current_file_->getFragment(off - pat_size_/2, pat_size_);
		buffs.emplace_back(buff, buff+pat_size_);
	}
	if (g_log_mode == I) cout << string(20, ' ') << '\r';
	return buffs;
}

patterns_t Mp4::offsToPatterns(const offs_t& all_offs, const string& load_prefix) {
	auto offs_to_consider = choose100(all_offs);
	auto buffs = offsToBuffs(offs_to_consider, load_prefix);
	auto patterns = genRawPatterns(buffs);

	auto offs_to_check = choose100(all_offs);
	auto buffs2 = offsToBuffs(offs_to_consider, load_prefix);
	countPatternsSuccess(patterns, buffs2);

//	for (auto& p : patterns) cout << p.successRate() << " " << p << '\n';

	filterBySuccessRate(patterns, load_prefix);
	return patterns;
}

void Mp4::genDynPatterns() {
	for (auto& t : tracks_) t.dyn_patterns_.resize(tracks_.size());

	for (auto const& kv: chunk_transitions_) {
		auto& patterns = tracks_[kv.first.first].dyn_patterns_[kv.first.second];
		string prefix = ss(kv.first.first, "->", kv.first.second, ": ");

		patterns = offsToPatterns(kv.second, prefix);
	}

	for (auto& t: tracks_) {
		t.genPatternPerm();
		if (!has_zero_transitions_ && t.hasZeroTransitions()) has_zero_transitions_ = true;
	}
	logg(V, "has_zero_transitions_: ", has_zero_transitions_, '\n');
	using_dyn_patterns_ = true;
}


void Mp4::noteUnknownSequence(off_t offset) {
	addToExclude(offset-unknown_length_, unknown_length_);
	unknown_lengths_.emplace_back(unknown_length_);
	unknown_length_ = 0;
}

void Mp4::addUnknownSequence(off_t start, uint64_t length) {
	assert(length);
	addToExclude(start, length);
	unknown_lengths_.emplace_back(length);
}

void Mp4::addToExclude(off_t start, uint64_t length, bool force) {
	if (g_dont_exclude && !force) return;
	assert(!current_mdat_->sequences_to_exclude_.size() ||
	       start > current_mdat_->sequences_to_exclude_.back().first);

	if (start + length > to_uint64(current_mdat_->contentSize())) {
		logg(V, start, " + ", length, " > ", current_mdat_->contentSize(), "\n");
		logg(W, "addToExclude: sequence goes beyond EOF\n");
		length = current_mdat_->contentSize() - start;
	}


	current_mdat_->sequences_to_exclude_.emplace_back(start, length);
	current_mdat_->total_excluded_yet_ += length;
}

FileRead& Mp4::openFile(const string& filename) {
	delete current_file_;
	current_file_ = new FileRead(filename);
	if (!current_file_->length())
		throw length_error(ss("zero-length file: ", filename));
	return *current_file_;
}

void Mp4::dumpIdxAndOff(off_t off, int idx) {
	auto real_off = current_mdat_->contentStart() + off;
	cout << setw(15) << ss("(", idx++, ") ") << setw(12) << ss(off, " / ")
	     << setw(8) << real_off << " : ";
}

void Mp4::chkExpectedOff(off_t* expected_off, off_t real_off, uint sz, int idx) {
	int v = real_off - *expected_off;
	if (v) {
		dumpIdxAndOff(*expected_off, --idx);
		cout << "unknown " << v << "\n";
	}
	*expected_off = real_off + sz;
}

void Mp4::dumpMatch(const FrameInfo& fi, int idx, off_t* expected_off) {
	if (expected_off) chkExpectedOff(expected_off, fi.offset_, fi.length_, idx);
	dumpIdxAndOff(fi.offset_, idx);
//	cout << fi << '\n';
	cout << fi;
	int comp_off = 0;
	auto& track = tracks_[fi.track_idx_];
	if (track.orig_comp_offs_.size())
		comp_off = track.orig_comp_offs_[track.dump_idx_++];
	cout << ", " << comp_off << '\n';
}

void Mp4::dumpChunk(const Mp4::Chunk& chunk, int& idx, off_t* expected_off) {
	if (expected_off) chkExpectedOff(expected_off, chunk.off_, chunk.size_, idx);
	dumpIdxAndOff(chunk.off_, idx);
	cout << chunk << '\n';
	idx += chunk.n_samples_;
}

void Mp4::setDummyIsSkippable() {
	if (idx_free_ < 0) return;
	auto& t = tracks_[idx_free_];
	logg(V, "running setDummyIsSkippable() ... \n");

	auto offsetChkWorks = [&]() {
		vector<pair<off_t, off_t>> tests;
	//	for (auto& c : t.chunks_) tests.emplace_back(c.off_, c.off_+c.size_);
		for (auto& c : t.chunks_) tests.emplace_back(c.off_ - current_mdat_->contentStart(), c.off_+c.size_ - current_mdat_->contentStart());
		auto chosen_tests = choose100(tests);
		logg(V, "chosen_tests.size(): ", chosen_tests.size(), '\n');
		for (auto& kv : tests) {
			off_t off = kv.first, off_end = kv.second;
			chkOffset(off);
			last_track_idx_ = -1;
	//		if (off != off_end) return false;
			if (off != off_end) {
				logg(V, "chkOffset failed: '", offToStr(off), "' != '", offToStr(off_end), "'\n");
				return false;
			}
		}
		return true;
	};

	if (offsetChkWorks()) {
		logg(V, "yes, via chkOffset\n");
		dummy_is_skippable_ = true;
	}
	else if (t.dummyIsUsedAsPadding()) {
		logg(V, "yes, by using padding-skip strategy\n");
		dummy_is_skippable_ = true;
		dummy_do_padding_skip_ = true;
	}
	else {
		logg(V, "no, seems not to be skippable\n");
		dummy_is_skippable_ = false;
	}
}

void Mp4::correctChunkIdx(int track_idx) {
	assert(track_idx >= 0 && track_idx != idx_free_);
	if (!track_order_.size()) return;

	// TODO: call correctChunkIdx once we know n_samples
	//       ATM result could be wrong if 'chunk::ĺikely_n_samples_.size() > 1'
	while (track_order_[chunk_idx_].first != track_idx) chunk_idx_++;

	if (tracks_[track_idx].likely_n_samples_.size() > 1)
		logg(W, "correctChunkIdx(", track_idx, ") could be wrong\n");
}

void Mp4::correctChunkIdxSimple(int track_idx) {
	assert(track_idx != idx_free_);
	auto order_sz = track_order_simple_.size();
	if (!order_sz) return;

	int off_ok = -1;
	for (uint off=0; off < order_sz; off++) {
		if (track_order_simple_[chunk_idx_+off % order_sz] == track_idx) {
			if (off_ok < 0) off_ok = off;
			else {logg(W, "correctChunkIdxSimple(", track_idx, "): next chunk is ambiguous\n"); break;};
		}
	}

	assert(off_ok >= 0);

	if (off_ok) {
		logg(V, "correctChunkIdxSimple(", track_idx, "): skipping ", off_ok, "chunks\n\n");
		chunk_idx_ += off_ok;
	}
}

int Mp4::skipNextZeroCave(off_t off, int max_sz, int n_zeros) {
	// skip to cave + skip over cave

	for (auto pos=off; max_sz > 0; pos+=n_zeros, max_sz-=n_zeros)
		if (isAllZerosAt(pos, n_zeros))
			for (; max_sz > 0; pos+=1, max_sz-=1)
				if (!isAllZerosAt(pos, 1)) return pos-off;
	return -1;
}

void Mp4::pushBackLastChunk() {
	if (last_track_idx_ >= 0)
		tracks_[last_track_idx_].pushBackLastChunk();
}

bool Mp4::isTrackOrderEnough() {
	auto isEnough = [&]() {
		if (!track_order_.size()) return false;
	    for (auto& t : tracks_) {
			if (t.isSupported()) continue;
			if (t.is_dummy_ && dummy_is_skippable_) continue;  // not included in track_order_
			if (!t.is_dummy_ && t.likely_sample_sizes_.size() == 1) continue;

			logg(W, "track_order_ found, but not sufficient\n");
			track_order_.clear();
			return false;
		}
		return true;
	};

	bool is_enough = isEnough();
	logg(V, "isTrackOrderEnough: ", is_enough, "  (sz=", track_order_.size(), ")\n");
	if (!is_enough && track_order_.size()) {
		// in theory this could probably still be used somehow..
		logg(W, "track_order_ found, but not sufficient\n");
		track_order_.clear();
	}
	return is_enough;
}

void Mp4::genLikelyAll() {
	for (auto& t: tracks_) t.genLikely();
}

void Mp4::genDynStats(bool force_patterns) {
	if (chunk_transitions_.size()) return;  // already generated
	if (!current_mdat_) findMdat(*current_file_);
	genChunks();

	genChunkTransitions();
	genTrackOrder();
	genLikelyAll();

	setDummyIsSkippable(); // note: genDynPatterns could (indirectly) change this outcome, via Mp4::has_zero_transitions_
	if (isTrackOrderEnough() && !force_patterns) {
		for (auto& t : tracks_)
			t.dyn_patterns_.resize(tracks_.size());  // prevent segfault
		return;
	}

	genDynPatterns();
}

void Mp4::checkForBadTracks() {
	if (track_order_.size()) return;  // we already checked via `isTrackOrderEnough()`
	for (auto& t: tracks_) {
		if (!t.codec_.isSupported() && !t.hasPredictableChunks() &&
		    !(t.is_dummy_ && dummy_is_skippable_)) {
			logg(ET, "bad track: '", t.codec_.name_, "'\n");
		}
	}
}

void Mp4::dumpSamples() {
	auto& mdat = current_mdat_;
	if (!mdat) {
		auto& file = openFile(filename_ok_);
		findMdat(file);
	}
	analyze(true);
	cout << filename_ok_ << '\n';

	int i = 0;
	if (to_dump_.size())
		for (auto const& x : to_dump_)
			dumpMatch(x, i++);
	else {
		auto frame_it = off_to_frame_.begin();
		auto chunk_it = off_to_chunk_.begin();
		auto frameItAtEnd = [&](){return frame_it == off_to_frame_.end();};
		auto chunkItAtEnd = [&](){return chunk_it == off_to_chunk_.end();};
		off_t expected_off = 0;
		while (true) {
			if (!chunkItAtEnd() && !frameItAtEnd()) {
				if (frame_it->second.offset_ < chunk_it->second.off_)
					dumpMatch((frame_it++)->second, i++, &expected_off);
				else
					dumpChunk((chunk_it++)->second, i, &expected_off);
			}
			else if (!frameItAtEnd())
				dumpMatch((frame_it++)->second, i++, &expected_off);
			else if (!chunkItAtEnd())
				dumpChunk((chunk_it++)->second, i, &expected_off);
			else
				break;
		}
	}
}

bool Mp4::findAtom(FileRead& file_read, string atom_name, Atom& atom) {
	while(atom.name_ != atom_name) {
		off_t new_pos = Atom::findNextAtomOff(file_read, &atom, true);
		if (new_pos >= file_read.length() || new_pos < 0) {
			logg(W, "start of ", atom_name, " not found\n");
			atom.start_ = -8;  // no header
			file_read.seek(0);
			return false;
		}
		file_read.seek(new_pos);
		atom.parseHeader(file_read);
	}
	return true;
}

BufferedAtom* Mp4::mdatFromRange(FileRead& file_read, BufferedAtom& mdat) {
	mdat.start_ = g_range_start - 8;
	mdat.name_ = "mdat";

	auto len = file_read.length();
	if (g_range_end > len) mdat.file_end_ = len;
	else if (g_range_end < 0) mdat.file_end_ = len + g_range_end;
	else mdat.file_end_ = g_range_end;

	logg(I, "range: ", g_range_start, ":", g_range_end, " -> ", mdat.start_, ":", mdat.file_end_, " (", mdat.contentSize(), ")\n");
	if (mdat.contentSize() <= 0)
		logg(ET, "bad range, contentSize: ", mdat.contentSize(), "\n");
	return &mdat;
}

BufferedAtom* Mp4::findMdat(FileRead& file_read) {
	delete current_mdat_;
	current_mdat_ = new BufferedAtom(file_read);
	auto& mdat = *current_mdat_;

	if (file_read.filename_ == filename_ok_) {
		Atom* p = root_atom_->atomByName("mdat", true);
		if (p) mdat.Atom::operator=(*p);
	}

	else if (g_range_start != kRangeUnset)
		return mdatFromRange(file_read, mdat);

	if (!isPointingAtAtom(file_read)) {
		logg(W, "no mp4-structure found in: '", file_read.filename_, "'\n");
		auto moov = root_atom_->atomByNameSafe("moov");
//		if (ftyp_ == "XAVC") {
		if (orig_mdat_start_ < moov->start_) {
			logg(I, "using orig_mdat_start_ (=", orig_mdat_start_, ")\n");
			mdat.start_ = orig_mdat_start_;
			mdat.name_ = "mdat";
		}
		else if (!g_search_mdat) {
			logg(I, "assuming start_offset=0. \n",
			     "      use '-sm' to search for 'mdat' atom instead (via brute-force)\n");
			mdat.start_ = -8;
			mdat.name_ = "mdat";
		}
	}

	findAtom(file_read, "mdat", mdat);
	mdat.file_end_ = file_read.length();

	return &mdat;
}

void Mp4::addFrame(const FrameInfo& fi) {
	Track& track = tracks_[fi.track_idx_];
	track.num_samples_++;

	if (fi.keyframe_)
		track.keyframes_.push_back(track.sizes_.size());

	if (fi.should_dump_)
		to_dump_.emplace_back(fi);

	if (fi.audio_duration_ && track.constant_duration_ == -1) track.times_.push_back(fi.audio_duration_);
	if (!track.constant_size_) track.sizes_.push_back(fi.length_);
}

void Mp4::addChunk(const Mp4::Chunk& chunk) {
	FrameInfo match(chunk.track_idx_, 0, 0, chunk.off_, chunk.sample_size_);
	for (uint n=chunk.n_samples_; n--;) {
		addFrame(match);
		match.offset_ += chunk.sample_size_;
	}
}

const uchar* Mp4::loadFragment(off_t offset, bool update_cur_maxlen) {
	if (update_cur_maxlen)
		current_maxlength_ = min((int64_t) max_part_size_, current_mdat_->contentSize() - offset);
	auto buf_sz = min((int64_t) g_max_buf_sz_needed, current_mdat_->contentSize() - offset);
	return current_fragment_ = current_mdat_->getFragment(offset, buf_sz);
}

bool Mp4::hasCodec(const string& codec_name) {
	for (auto& t : tracks_)
		if (t.codec_.name_ == codec_name) return true;
	return false;
}

uint Mp4::getTrackIdx(const string& codec_name) {
	int r = getTrackIdx2(codec_name);
	if (r < 0) throw "asked for nonexistent track";
	return r;
}

int Mp4::getTrackIdx2(const string& codec_name) const {
	for (uint i=0; i < tracks_.size(); i++)
		if (tracks_[i].codec_.name_ == codec_name) return i;
	return -1;
}

string Mp4::getCodecName(uint track_idx) {
	if (track_idx >= tracks_.size()) return "????";
	return tracks_[track_idx].codec_.name_;
}

Track& Mp4::getTrack(const string& codec_name) {
	auto idx = getTrackIdx(codec_name);
	return tracks_[idx];
}

// currently only used to distinguish duplicate tracks (e.g. 2x mp4a)
bool Mp4::isExpectedTrackIdx(int i) {
	if (track_order_simple_.empty()) return true;
	int expected_idx = track_order_simple_[chunk_idx_ % track_order_simple_.size()];
	if (expected_idx == i) return true;

	if (getCodecName(i) != getCodecName(expected_idx)) {
		logg(W, "expected codec ", getCodecName(expected_idx), " but found ",  getCodecName(i), "\n");
		correctChunkIdxSimple(i);
		return true;
	}
	return false;
}

bool Mp4::pointsToZeros(off_t off) {
	if (current_mdat_->contentSize() - off < 4) return false;
	auto buff = current_mdat_->getFragment(off, 4);
	if (*(int*)buff == 0) {
		logg(V, "pointsToZeros: found 4 zero bytes at ", offToStr(off), "\n");
		return true;
	}
	return false;
}

bool Mp4::shouldBeStrict(off_t off, int track_idx) {
	auto& t = tracks_[track_idx];
	if (!unknown_length_ || !t.isSupported()) return false;
	if (!g_use_chunk_stats || !tracks_.back().is_dummy_ || !t.chunkMightBeAtAnd() || !t.isChunkOffsetOk(off)) return true;

	assert(last_track_idx_ == idx_free_);
	auto buff = getBuffAround(off, Mp4::pat_size_);
	if (!buff) return true;

	for (auto& p : tracks_.back().dyn_patterns_[track_idx]) {
		if (p.doesMatch(buff)) {
			logg(V, "won't be strict since a free_", t.codec_.name_, " pattern matched at ", off, "\n");
			return false;
		}
	}
	return true;

}

bool Mp4::wouldMatch(off_t offset, const string& skip, bool force_strict, int last_track_idx) {
	auto chkChunkOffOk = [&](uint i) -> bool {
		if (g_use_chunk_stats && to_uint(last_track_idx) != i && !tracks_[i].isChunkOffsetOk(offset)) {
			logg(V, "would match, but offset is not accepted as start-of-chunk by '", tracks_[i].codec_.name_, "'\n");
			return false;
		}
		else logg(V, "chunkOffOk: ", offset, " ok for ", tracks_[i].codec_.name_, "\n");
		return true;
	};

	auto start = loadFragment(offset);
	for (uint i=0; i < tracks_.size(); i++) {
		auto& c = tracks_[i].codec_;
		bool be_strict = force_strict || shouldBeStrict(offset, i);
		if (tracks_[i].codec_.name_ == skip) continue;
		if (be_strict && !c.matchSampleStrict(start)) continue;
		if (!be_strict && !c.matchSample(start)) continue;

		logg(V, "wouldMatch(", offset, ", \"", skip, "\", ", force_strict, ") -> yes, ", c.name_, "\n");
		return chkChunkOffOk(i);
	}

	if (g_use_chunk_stats) {
		auto last_idx = last_track_idx >= 0 ? last_track_idx : last_track_idx_;
		if (last_idx < 0) return false;
		if (wouldMatchDyn(offset, last_idx)) {
			logg(V, "wouldMatch(", offset, ", \"", skip, "\", ", force_strict, ") -> yes\n");
			return true;
		}
	}

	logg(V, "wouldMatch(", offset, ", \"", skip, "\", ", force_strict, ") -> no\n");
	return false;
}

bool Mp4::wouldMatch2(const uchar *start) {
	for (auto& t : tracks_) if (t.codec_.matchSample(start)) return true;
	return false;
}


FrameInfo Mp4::getMatch(off_t offset, bool force_strict) {
	auto start = loadFragment(offset);

	// hardcoded match
	if (pkt_idx_ == 4 && hasCodec("tmcd")) {
		if (!wouldMatch(offset, "", true, last_track_idx_)) {
			logg(V, "using hardcoded 'tmcd' packet (len=4)\n");
			return FrameInfo(getTrackIdx("tmcd"), false, 0, offset, 4);
		}
		else {
			logg(W2, "hardcoded tmcd as 4th packet seems wrong..\n");
		}
	}

	for (uint i=0; i < tracks_.size(); i++) {
		auto& track = tracks_[i];
		Codec& c = track.codec_;
		logg(V, "Track codec: ", c.name_, '\n');
		if (g_use_chunk_stats && (to_uint(last_track_idx_) != i || unknown_length_) && !track.isChunkOffsetOk(offset)) {
			logg(V, "offset not accepted as start-of-chunk offset by '", track.codec_.name_, "'\n");
			continue;
		}

		bool be_strict = force_strict || shouldBeStrict(offset, i);

		if (be_strict && !c.matchSampleStrict(start)) continue;
		if (!be_strict && !c.matchSample(start)) continue;

		uint length;
		int length_signed;
		if (track.constant_size_ > 0) {
			length = track.constant_size_;
			goto after_length_chk;
		}
		length_signed = c.getSize(start, current_maxlength_, offset);
		length = to_uint(length_signed);

		logg(V, "part-length: ", length_signed, '\n');
		if(length_signed < 1) {
			logg(V, "Invalid length: part-length is ", length_signed, '\n');
			continue;
		}
		if(length > current_maxlength_) {
			logg(V, "limit: ", min(max_part_size_, current_maxlength_), "\n");
			logg(E, "Invalid length: ", length, " - too big (track: ", i, ")\n");
			continue;
		}
		if (length  <  6 && c.name_ == "avc1") {  // very short NALs are ok, short frames aren't
			logg(W2, "Invalid length: ", length, " - too small (track: ", i, ")\n");
			continue;
		}

        after_length_chk:
		if (c.was_bad_) {
			logg(W, "Codec::was_bad_ = 1\n");
//			logg(V, "Codec::was_bad_ = 1 -> skipping\n");
//			continue;
		}

		if (track.has_duplicates_ && !isExpectedTrackIdx(i)) continue;

		if (c.name_ == "jpeg" && track.end_off_gcd_) {
			length += track.stepToNextOtherChunk(offset + length);
		}

		return FrameInfo(i, c, offset, length);
	}

	if (idx_free_ > 0 && dummy_do_padding_skip_) {  // next chunk is probably padded with random data..
		int len = tracks_[idx_free_].stepToNextOtherChunk(offset);
		return FrameInfo(idx_free_, false, 0, offset, len);
	}

	return FrameInfo();
}

bool Mp4::transitionIsUnclear(int track_idx_a, int track_idx_b) {
	auto& ta = tracks_[track_idx_a], tb = tracks_[track_idx_b];
	if (tb.isSupported()) return false;
	if (ta.dyn_patterns_[track_idx_b].empty() &&
	    chunk_transitions_[{track_idx_a, track_idx_b}].size() > 5) {
		return true;
	}
	return false;
}

bool Mp4::hasUnclearTransitions(int track_idx_a) {
	for (uint i=0; i < tracks_.size(); i++)
		if (transitionIsUnclear(track_idx_a, i)) return true;
	return false;
}

bool Mp4::wouldMatchDyn(off_t offset, int last_idx) {
	auto new_track = tracks_[last_idx].useDynPatterns(offset);
	if (new_track >= 0) {
		logg(V, "wouldMatchDyn(", offset, ", ", last_idx, ") -> yes (", getCodecName(last_idx), "_", getCodecName(new_track), ")\n");
		return true;
	}
	logg(V, "wouldMatchDyn(", offToStr(offset), ", ", last_idx, ") -> no\n");
	return false;
}

bool Mp4::anyPatternMatchesHalf(off_t offset, uint track_idx_to_try) {
	auto buff = g_mp4->current_mdat_->getFragment(offset, Mp4::pat_size_ / 2);
	if (!buff) return false;

	for (auto& t : tracks_) {
		for (auto& p : t.dyn_patterns_[track_idx_to_try]) {
			if (g_log_mode >= V) {
				cout << string(36, ' ');
				printBuffer(buff, Mp4::pat_size_);
				cout << p << '\n';
			}
			if (p.doesMatchHalf(buff)) return true;
		}
	}
	return false;

}

Mp4::Chunk Mp4::fitChunk(off_t offset, uint track_idx_to_fit, uint known_n_samples) {
	Mp4::Chunk c;
	auto& t = tracks_[track_idx_to_fit];
	if (!t.hasPredictableChunks()) return c;

	for (auto n_samples : t.likely_n_samples_) {
		if (known_n_samples) n_samples = known_n_samples;
		for (auto s_sz : t.likely_sample_sizes_) {
			auto dst_off = offset + n_samples*s_sz;
			if (dst_off < current_mdat_->contentSize() && wouldMatch(dst_off, "", false, track_idx_to_fit)) {
				assert(n_samples > 0);
				c = Chunk(offset, n_samples, track_idx_to_fit, s_sz);
				return c;
			}
		}
		if (known_n_samples) break;
	}
	return c;
}

int Mp4::getLikelyNextTrackIdx(int* n_samples) {
	auto p = track_order_[chunk_idx_ % track_order_.size()];
	if (n_samples) *n_samples = p.second;
	return p.first;
}

int Mp4::calcFallbackTrackIdx() {
	if (!using_dyn_patterns_) return -1;
	for (uint i=0; i < tracks_.size(); i++) {
		auto& t = tracks_[i];
		if (!t.isChunkTrack()) continue;

		for (auto& t2 : tracks_) {
			if (t2.dyn_patterns_[i].size()) continue;
		}

		return i;
	}
	return -1;
}

Mp4::Chunk Mp4::getChunkPrediction(off_t offset, bool only_perfect_fit) {
	logg(V, "called getChunkPrediction(", offToStr(offset), ") ... \n");
	Mp4::Chunk c;
	if (last_track_idx_ == kDefaultFreeIdx) return c;  // could try all instead..
	int track_idx;
	if (track_order_.size()) {
		int n_samples;
		track_idx = getLikelyNextTrackIdx(&n_samples);
		auto& track = tracks_[track_idx];
		if (!track.hasPredictableChunks()) return c;
		else if (track.likely_sample_sizes_.size() > 1) {
			c = fitChunk(offset, track_idx, n_samples);
			if (!c) logg(V, "fitChunk() failed despite supposedly known (track_idx, n_samples) = ", track_idx, ", ", n_samples, "\n");
			return c;
		}
		else {
			auto s_sz = track.likely_sample_sizes_[0];
			if (n_samples * s_sz > current_mdat_->contentSize() - offset) return c;
			c = Chunk(offset, n_samples, track_idx, s_sz);
			logg(V, "chunk derived from track_order_: ", c, "\n");
			return c;
		}
	}

	if (last_track_idx_ == -1) {  // very first offset
		track_idx = getTrackIdx(orig_first_track_->codec_.name_);
		logg(V, "orig_trak:", orig_first_track_->codec_.name_, " ", track_idx, '\n');
		if (orig_first_track_->codec_.name_ == "priv") {  // FC7203 adds 241 zeros after second thumbnail
			logg(V, "using skipNextZeroCave .. \n");
			int sz = skipNextZeroCave(offset, 1<<21, 12) - 1;
			logg(V, "skipNextZeroCave(", offset, ") -> ", sz, "\n");
			if (sz >= 0) return Chunk(offset, 1, track_idx, sz);
		}
		if (!anyPatternMatchesHalf(offset, track_idx)) {
			if (fallback_track_idx_ >= 0) {
				logg(V, "using fallback track\n");
				track_idx = fallback_track_idx_;
			}
			else {
				cout << "no half-pattern suggests this ..\n";
				return c;
			}
		}
	}
	else {
		track_idx = tracks_[last_track_idx_].useDynPatterns(offset);
//		logg(V, "found track_idx: ", track_idx, "\n");
	}

	if (track_idx >= 0 && !tracks_[track_idx].shouldUseChunkPrediction()) {
		logg(V, "should not use chunk prediction for '", getCodecName(track_idx), "'\n");
		return c;
	}
	else if (track_idx < 0) {
//		logg(V, "no chunk with future found\n");
		logg(V, "found no plausible chunk-transition pattern-match\n");
		return c;
	}

	auto& t = tracks_[track_idx];
	logg(V, "transition pattern ", getCodecName(last_track_idx_), "_", t.codec_.name_, " worked\n");

	if ((c = fitChunk(offset, track_idx))) {
		logg(V, "chunk found: ", c, "\n");
		return c;
	}

	if (only_perfect_fit) return c;

	// we boldly assume that track_idx was correct
	auto s_sz = t.likely_sample_sizes_[0];
	if (t.likely_n_samples_.empty()) {assert(false); return c;}
	auto n_samples = t.likely_n_samples_[0];  // smallest sample_size
	if (t.likely_n_samples_p < 0.9) {
		const int min_sz = 64;  // min assumed chunk size
		int new_n_samples = max(1, min_sz / s_sz);
		logg(V, "reducing n_sample ", n_samples, " -> ", new_n_samples, " because unsure\n");
		n_samples = new_n_samples;
	}
	n_samples = min(n_samples, int((current_mdat_->contentSize() - offset) / s_sz));

	bool at_end = n_samples < t.likely_n_samples_[0];
	if (!at_end) {
		logg(W2, "found chunk has no future: ", c, " at ", offToStr(c.off_ + c.size_), "\n");
		return c;
	}

	c = Chunk(offset, n_samples, track_idx, s_sz);
	assert(0 <= c.track_idx_ && to_size_t(c.track_idx_) < tracks_.size());
	return c;
}

const uchar* Mp4::getBuffAround(off_t offset, int64_t n) {
	auto& mdat = g_mp4->current_mdat_;
	if (offset - n/2 < 0 || offset + n/2 > mdat->contentSize()) return nullptr;
	return mdat->getFragment(offset - n/2, n);
}

bool Mp4::needDynStats() {
	if (g_use_chunk_stats) return true;
	for (auto& t: tracks_)
		if (!t.isSupported()) {
			logg(I, "unknown track '", t.codec_.name_, "' found -> fallback to dynamic stats\n");
			return true;
		}
	return false;
}

bool Mp4::chkNeedOldApi() {
	if (!is_new_ffmpeg_api) return false;
	vector<string> v = {"mp4v"};
	for (auto& t: tracks_)
		if (contains(v, t.codec_.name_)) {
			logg(E, "track '", t.codec_.name_, "' is not supported with ffmpeg > 3.3!\n");
			return true;
		}
	return false;
}

FrameInfo::FrameInfo(int track_idx, Codec& c, off_t offset, uint length)
    : track_idx_(track_idx), keyframe_(c.was_keyframe_), audio_duration_(c.audio_duration_),
      offset_(offset), length_(length), should_dump_(c.should_dump_) {}

FrameInfo::FrameInfo(int track_idx, bool was_keyframe, uint audio_duration, off_t offset, uint length)
    : track_idx_(track_idx), keyframe_(was_keyframe), audio_duration_(audio_duration),
      offset_(offset), length_(length), should_dump_(false) {}

FrameInfo::operator bool() const {
	return length_;
}

bool operator==(const FrameInfo& a, const FrameInfo& b) {
	return a.length_ == b.length_ && a.track_idx_ == b.track_idx_ &&
	    a.keyframe_ == b.keyframe_;
//	    a.keyframe_ == b.keyframe_ && a.audio_duration_ == b.audio_duration_;

}
bool operator!=(const FrameInfo& a, const FrameInfo& b) { return !(a == b); }

ostream& operator<<(ostream& out, const FrameInfo& fi) {
	auto cn = g_mp4->getCodecName(fi.track_idx_);
	return out << ss("'", cn, "', ", fi.length_, ", ", fi.keyframe_, ", ", fi.audio_duration_);
}

Mp4::Chunk::Chunk(off_t off, int ns, int track_idx, int sample_size)
    : Track::Chunk (off, ns * sample_size, ns), track_idx_(track_idx), sample_size_(sample_size) {}

ostream& operator<<(ostream& out, const Mp4::Chunk& c) {
	return out << ss("'", g_mp4->getCodecName(c.track_idx_), "' (", c.sample_size_, " x", c.n_samples_, ")");
}

bool operator==(const Mp4::Chunk& a, const Mp4::Chunk& b) {
	return a.off_ == b.off_ && a.n_samples_  == b.n_samples_ &&
	        a.track_idx_ == b.track_idx_ && a.n_samples_ == b.n_samples_ &&
	        a.size_ == b.size_;
}
bool operator!=(const Mp4::Chunk& a, const Mp4::Chunk& b) { return !(a == b); }


int64_t Mp4::calcStep(off_t offset) {
	int64_t step = numeric_limits<int64_t>::max();
	if (g_use_chunk_stats) {
		step = numeric_limits<int64_t>::max();
		for (auto& t : tracks_) {
			if (t.is_dummy_) continue;
			step = min(step, t.stepToNextOwnChunk(offset));
		}

		step = min(step, current_mdat_->contentSize() - offset);
	}
	else {
		step = Mp4::step_;
	}
	return step;
}

bool Mp4::isAllZerosAt(off_t off, int n) {
	if (current_mdat_->contentSize() - off < n) return false;
	auto buff = current_mdat_->getFragment(off, 4);
	if (isAllZeros(buff, n)) {
		logg(V, "isAllZerosAt: found ", n, " zero bytes at ", offToStr(off), "\n");
		return true;
	}
	return false;
}

bool Mp4::chkOffset(off_t& offset) {
start:
	if (offset >= current_mdat_->contentSize()) {  // at end?
		pushBackLastChunk();
		if (unknown_length_) noteUnknownSequence(offset);
		return false;
	}


	auto start = loadFragment(offset);
	uint begin = *(uint*)start;

	static uint loop_cnt = 0;
	if (g_log_mode == I && loop_cnt++ % 2000 == 0) outProgress(offset, current_mdat_->file_end_);

	auto shouldKeepZeros = [&]() {
		if (g_use_chunk_stats) {
			for (auto& cn : {"sowt", "twos"}) {
				if (hasCodec(cn) && getTrack(cn).isChunkOffsetOk(offset)) {
					logg(V, "won't skip zeros at: ", offToStr(offset), "\n");
					return true;
				}
			}

			if (has_zero_transitions_) {
				auto c = getChunkPrediction(offset, true);
				if (c) {
					logg(V, "won't skip zeros at: ", offToStr(offset), " (perfect chunk-match)\n");
					return true;
				}
			}
		}
		return false;
	};

	if (*(int*)start == 0 && !shouldKeepZeros()) {
		logg(V, "skipping zeros at: ", offToStr(offset), "\n");
		if (track_order_.size()) {
			int idx = getLikelyNextTrackIdx();
			int step = tracks_[idx].stepToNextOwnChunkAbs(offset);
			logg(V, "used stepToNextOwnChunkAbs of likelyNextTrack -> ", step, "\n");
			if (step > 4) {
				offset += step;
				return true;
			}
		}
		if (idx_free_ > 0 && dummy_do_padding_skip_) {  // next chunk is probably padded with random data..
			int len = tracks_[idx_free_].stepToNextOtherChunk(offset);
			logg(V, "used free's end_off_gcd -> ", len, "\n");
			offset += len;
			if (len) return true;
			else logg(W, "end_off_gcd method failed..\n");
		}
		int64_t step = 4;
		if (unknown_length_ || g_use_chunk_stats) step = calcStep(offset);
		if (unknown_length_) unknown_length_ += step;
		else if (idx_free_ > 0 && tracks_[last_track_idx_].is_dummy_) {
			auto& c = tracks_[idx_free_].current_chunk_;
			c.size_ += step;
			c.n_samples_ += step;  // sample size is 1
		}
		offset += step;
		goto start;
	}

	// skip 'mdat' headers
	if (string(start+4, start+8) == "mdat") {
		if (unknown_length_) noteUnknownSequence(offset);
		if (idx_free_ >= 0 && last_track_idx_ != idx_free_) {
			pushBackLastChunk();
			last_track_idx_ = idx_free_;
		}

		addToExclude(offset, 8);
		logg(V, "Skipping 'mdat' header: ", offToStr(offset), '\n');
		offset += 8;
		goto start;
	}

	// skip atoms (e.g. moov, free)
	if (isValidAtomName(start+4)) {
		if (unknown_length_) noteUnknownSequence(offset);
		pushBackLastChunk();
		last_track_idx_ = idx_free_;
		uint atom_len = swap32(begin);
		string s = string(start+4, start+8);
		if (offset + atom_len <= current_mdat_->contentSize()) {
			logg(!contains({"free", "iidx"}, s) ? W : V, "Skipping ", s, " atom: ", atom_len, '\n');
			addToExclude(offset, atom_len, true);
			offset += atom_len;
			goto start;
		}
		else {
			logg(W, "NOT skipping ", s, " atom: ", atom_len, " (at ", offToStr(offset), ")\n");
		}
	}


	if (g_log_mode >= LogMode::V) {
		logg(V, "\n(reading element from mdat)\n");
		printOffset(offset);
	}

	return true;
}

void Mp4::printOffset(off_t offset) {
	auto s = current_mdat_->getFragment(offset, min((int64_t)8, current_mdat_->contentSize() - offset));

	uint begin = swap32(*(uint*)s);
	uint next = swap32(*(uint*)(s+4));
	logg(V, "Offset: ", offToStr(offset), " : ", setfill('0'), setw(8), hex, begin, " ", setw(8), next, dec, '\n');
}

void Mp4::chkDetectionAtImpl(FrameInfo* detectedFramePtr, Mp4::Chunk* detectedChunkPtr, off_t off) {
	auto correctFrameIt = off_to_frame_.end();
	auto correctChunkIt = off_to_chunk_.end();
	if (detectedFramePtr) {
		if ((correctFrameIt = off_to_frame_.find(off)) == off_to_frame_.end())
			correctChunkIt = off_to_chunk_.find(off);
	}
	else {
		if ((correctChunkIt = off_to_chunk_.find(off)) == off_to_chunk_.end())
			correctFrameIt = off_to_frame_.find(off);
	}

	bool correct,
	    correctFrameFound = correctFrameIt != off_to_frame_.end(),
	    correctChunkFound = correctChunkIt != off_to_chunk_.end();

	if (detectedFramePtr) correct = correctFrameFound && *detectedFramePtr == correctFrameIt->second;
	else if (detectedChunkPtr) correct = correctChunkFound && *detectedChunkPtr == correctChunkIt->second;
	else correct = !correctFrameFound && !correctChunkFound;

	if (correct) return;
//	if (correctFrameFound && detectedFramePtr && detectedFramePtr->keyframe_) return;

	auto coutExtraInfo = [&]() {
		if (correctFrameFound) {
			cout << ", chunk " << tracks_[correctFrameIt->second.track_idx_].chunks_.size()
			 << ", pkt_in_chunk " << tracks_[correctFrameIt->second.track_idx_].current_chunk_.n_samples_;
		}
		else if (correctChunkFound) {
			 cout << ", chunk " << tracks_[correctChunkIt->second.track_idx_].chunks_.size();
		}
		cout << "):\n";
	};

	cout << "bad detection (at " << offToStr(off) << ", chunk " << chunk_idx_ << ", pkt " << pkt_idx_;
	coutExtraInfo();

	if (detectedFramePtr) cout << "  detected: " << *detectedFramePtr << '\n';
	else if (detectedChunkPtr) cout << "  detected: " << *detectedChunkPtr << '\n';
	else cout << "  detected: (none)\n";

	if (correctFrameFound) cout << "  correct: " << correctFrameIt->second << '\n';
	else if (correctChunkFound) cout << "  correct: " << correctChunkIt->second << '\n';
	else cout << "  correct: (none)\n";

	hitEnterToContinue();
}

void Mp4::chkFrameDetectionAt(FrameInfo& detected, off_t off) { chkDetectionAtImpl(&detected, nullptr, off); }
void Mp4::chkChunkDetectionAt(Mp4::Chunk& detected, off_t off) { chkDetectionAtImpl(nullptr, &detected, off); }

bool Mp4::tryChunkPrediction(off_t& offset) {
	if (!g_use_chunk_stats) return false;

	Mp4::Chunk chunk = getChunkPrediction(offset);
	if (chunk) {
		auto& t = tracks_[chunk.track_idx_];

		if (use_offset_map_) chkChunkDetectionAt(chunk, offset);

		if (unknown_length_ && t.is_dummy_) {
			logg(V, "found '", t.codec_.name_, "' chunk inside unknown sequence: ", chunk, "\n");
			unknown_length_ += chunk.size_;
		}
		else if (unknown_length_) {
			noteUnknownSequence(offset);
			logg(V, "found healthy chunk again: ", chunk, "\n");
			correctChunkIdx(chunk.track_idx_);
			disableNoiseBuffer();
		}

		pushBackLastChunk();
		if (chunk.track_idx_ != idx_free_) {
			if (!first_chunk_found_) onFirstChunkFound(chunk.track_idx_);
			chunk_idx_++;
		}

		t.current_chunk_ = chunk;
		t.current_chunk_.already_excluded_ = current_mdat_->total_excluded_yet_;

		if (!t.is_dummy_) addChunk(chunk);

		last_track_idx_ = chunk.track_idx_;
		pkt_idx_ += chunk.n_samples_;
		offset += chunk.size_;

		return true;
	}
	return false;
}

bool Mp4::shouldPreferChunkPrediction() {
	return g_use_chunk_stats &&
	            ((last_track_idx_ >= 0 && tracks_[last_track_idx_].chunkMightBeAtAnd()) ||
	            (last_track_idx_ == -1 && orig_first_track_->shouldUseChunkPrediction())
	            );
}

off_t Mp4::toAbsOff(off_t offset) {
	return current_mdat_->contentStart() + offset;
}

string Mp4::offToStr(off_t offset) {
	return ss(offset, " / " , toAbsOff(offset));
}

void Mp4::onFirstChunkFound(int track_idx) {
	if (track_idx == idx_free_) return;
	first_chunk_found_ = true;
	assert(chunk_idx_ == 0);
	correctChunkIdxSimple(track_idx);
	if (chunk_idx_)
		logg(W, "different start chunk: ", track_idx, " instead of ", track_order_simple_[0], "\n");
}

bool Mp4::tryMatch(off_t& offset) {
	FrameInfo match = getMatch(offset);
	if (match) {
		auto& t = tracks_[match.track_idx_];

		if (use_offset_map_) chkFrameDetectionAt(match, offset);

		if (unknown_length_) {
			noteUnknownSequence(offset);
			logg(V, "found healthy packet again: ", match, "\n");
			correctChunkIdx(match.track_idx_);
			disableNoiseBuffer();
		}

		if (!first_chunk_found_) onFirstChunkFound(match.track_idx_);
		if (last_track_idx_ != match.track_idx_) {
			if (match.track_idx_ != idx_free_) chunk_idx_++;
			pushBackLastChunk();
			t.current_chunk_.off_ = offset;
			t.current_chunk_.already_excluded_ = current_mdat_->total_excluded_yet_;
		}

		if (t.has_duplicates_ && t.chunkReachedSampleLimit()) {
			pushBackLastChunk(); chunk_idx_++;
		}

		addFrame(match);

		t.current_chunk_.n_samples_++;
		logg(V, t.current_chunk_.n_samples_, "th sample in ", t.chunks_.size()+1, "th ", t.codec_.name_, "-chunk\n");
		last_track_idx_ = match.track_idx_;
		offset += match.length_;

		pkt_idx_++;


		return true;
	}
	return false;
}

string rewriteDestination(const std::string& dst) {
	if (!g_dst_path.size()) return dst;
	if (isdir(g_dst_path)) return g_dst_path + "/" + myBasename(dst);
	else return g_dst_path;
}

string Mp4::getPathRepaired(const std::string& ok, const std::string& corrupt) {
	auto filename_fixed = corrupt + "_fixed" + getOutputSuffix() + getMovExtension(ok);
	return rewriteDestination(filename_fixed);
}

bool Mp4::alreadyRepaired(const std::string& ok, const std::string& corrupt) {
	if (!g_skip_existing) return false;
	auto dst = getPathRepaired(ok, corrupt);
	if (FileRead::alreadyExists(dst)) {
		if (g_log_mode > E) cout << "exists: " << dst << '\n';
		return true;
	}
	return false;
}

bool Mp4::setDuplicateInfo() {
	map<string, int> cnt;
	bool foundAny = false;
	for (auto& t : tracks_) {
		if (!t.isSupported()) continue;
		cnt[t.codec_.name_]++;
	}
	for (auto& t : tracks_) {
		if (cnt[t.codec_.name_] > 1) {
			foundAny = true;
			t.has_duplicates_ = true;
			t.genLikely();
		}
	}
	return foundAny;
}

void Mp4::repair(const string& filename) {
	if (chkNeedOldApi()) {
		cout << "Help: You need to build against ffmpeg 3.3.\n"
		     << "      See the README.md on how to do that.\n";
		return;
	}
	if (needDynStats()) {
		g_use_chunk_stats = true;
		genDynStats();
		checkForBadTracks();
		if (g_log_mode >= LogMode::V) printDynStats();
		logg(I, "using dynamic stats, use '-is' to see them\n");
	}
	else if (setDuplicateInfo()) {;
		genTrackOrder();
		if (track_order_simple_.empty()) {
			logg(W, "duplicate codecs found, but no (simple) track order found\n");
		}
	}

	if (!g_ignore_unknown && max_part_size_ < g_max_partsize_default) {
		double x = (double)max_part_size_ / g_max_partsize_default;
		logg(V, "ss: reset to default (from ",  max_part_size_, " ~= ", setprecision(2), x, "*default)\n");
		max_part_size_ = g_max_partsize_default;
	}
	logg(V, "ss: max_part_size_: ", max_part_size_, "\n");

	if (alreadyRepaired(filename_ok_, filename)) exit(0);

	fallback_track_idx_ = calcFallbackTrackIdx();
	logg(V, "fallback: ", fallback_track_idx_, "\n");

	auto& file_read = openFile(filename);

	// TODO: What about multiple mdat?

	logg(V, "calling findMdat on truncated file..\n");
	auto mdat = findMdat(file_read);
	logg(I, "reading mdat from truncated file ...\n");

	if (file_read.length() > (1LL<<32)) {
		broken_is_64_ = true;
		logg(I, "using 64-bit offsets for the broken file\n");
	}

	use_offset_map_ = use_offset_map_ || filename == filename_ok_;
	if (use_offset_map_) analyze(true);

	duration_ = 0;
	for(uint i=0; i < tracks_.size(); i++)
		tracks_[i].clear();

	off_t offset = 0;

	if (g_use_chunk_stats) {
		auto first_off_abs = first_off_abs_ - mdat->contentStart();
		if (first_off_abs > 0 && wouldMatch(first_off_abs)) offset = first_off_abs;
		else if (wouldMatch(first_off_rel_)) offset = first_off_rel_;
		if (offset) {
			logg(V, "beginning at offset ", offToStr(offset), " instead of 0\n");
			addUnknownSequence(0, offset);
		}
	}

	while (chkOffset(offset)) {
		if (shouldPreferChunkPrediction()) {
			logg(V, "trying chunkPredict first.. \n");
			if (tryChunkPrediction(offset) || tryMatch(offset)) continue;
		}
		else {
			if (tryMatch(offset) || tryChunkPrediction(offset)) continue;
		}

		if (!unknown_length_) {
			pushBackLastChunk();
			last_track_idx_ = idx_free_;  // negative is fine
		}

		chkDetectionAtImpl(nullptr, nullptr, offset);

		if (g_ignore_unknown) {
			if (!g_muted && g_log_mode < LogMode::V) {
				mute();
			}
			else if (!g_noise_buffer_active && g_log_mode >= LogMode::V && !g_dont_omit) {
				logg(V, "unknown sequence -> enabling noise buffer ..\n");
				enableNoiseBuffer();
				mute();  // ffmpeg warnings are mostly noise and unhelpful without knowing file offset
			}

			auto step = calcStep(offset);
			unknown_length_ += step;
			offset += step;
		}
		else {
			if (g_muted) unmute();
			double percentage = (double)100 * offset / mdat->contentSize();
			mdat->file_end_ = toAbsOff(offset);
			mdat->length_ = offset + 8;

			logg(E, "unable to find correct codec -> premature end", " (~", setprecision(4), percentage, "%)\n",
			"       try '-s' to skip unknown sequences\n\n");
			logg(V, "mdat->file_end: ", mdat->file_end_, '\n');

			premature_percentage_ = percentage; premature_end_ = true;
			break;
		}
	}

	if (g_muted) unmute();

	for (auto& track : tracks_) track.fixTimes();

	auto filename_fixed = getPathRepaired(filename_ok_, filename);
	saveVideo(filename_fixed);
}

