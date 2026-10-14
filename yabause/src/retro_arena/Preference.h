/*  Copyright 2018 devMiyax(smiyaxdev@gmail.com)

This file is part of YabaSanshiro.

Yabause is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Yabause is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <string>
#include <set>
using std::string;

#include <nlohmann/json.hpp>
using json = nlohmann::json;


class Preference {
public:
    Preference( const string & filename );
    ~Preference();
    int getInt( const string & key, int defaultv );
    /* Game config, else ~/.yabasanshiro/default.config (global), else defaultv. Creates no keys. */
    int getIntGlobal( const string & key, int defaultv );
    bool getBool( const string & key, bool defaultv );
    /* Save only on a real change (setInt/setBool). */
    bool geaendert = false;
    /* String values, e.g. the configurable save state location ("state path").
       If the key is missing or has the wrong type the default is returned.
       Like getInt/getBool, the default is NOT written back to the file, so a
       key missing from a config never appears there but still takes effect
       with its default value. */
    string getString( const string & key, const string & defaultv );
    void setInt( const string & key, int value );
    void setBool( const string & key, bool value );

protected:
  string filename;
  json j;

  /* Keys that were actually set during this run. Only these are written to
     the file; everything else stays as it is on disk. */
  std::set<string> geaenderte_schluessel;
  void schreibe_nur_geaenderte();

  /* Whether the config file existed at startup. */
  bool gab_es_schon = false;
  void lege_standard_an();

};