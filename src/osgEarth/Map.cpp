/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2016 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/Map>
#include <osgEarth/MapFrame>
#include <osgEarth/MapModelChange>
#include <osgEarth/Registry>
#include <osgEarth/TileSource>
#include <osgEarth/HeightFieldUtils>
#include <osgEarth/URI>
#include <osgEarth/ElevationPool>
#include <iterator>

using namespace osgEarth;

#define LC "[Map] "

//------------------------------------------------------------------------

Map::ElevationLayerCB::ElevationLayerCB(Map* map) :
_map(map)
{
    //nop
}

void
Map::ElevationLayerCB::onVisibleChanged(TerrainLayer* layer)
{
    osg::ref_ptr<Map> map;
    if ( _map.lock(map) )
    {
        _map->notifyElevationLayerVisibleChanged(layer);
    }
}

//------------------------------------------------------------------------

Map::Map() :
osg::Object(),
_dataModelRevision(0)
{
    ctor();
}

Map::Map( const MapOptions& options ) :
osg::Object(),
_mapOptions          ( options ),
_initMapOptions      ( options ),
_dataModelRevision   ( 0 )
{
    ctor();
}

void
Map::ctor()
{
    // Set the object name.
    osg::Object::setName("osgEarth.Map");

    // Generate a UID.
    _uid = Registry::instance()->createUID();

    // If the registry doesn't have a default cache policy, but the
    // map options has one, make the map policy the default.
    if (_mapOptions.cachePolicy().isSet() &&
        !Registry::instance()->defaultCachePolicy().isSet())
    {
        Registry::instance()->setDefaultCachePolicy( _mapOptions.cachePolicy().get() );
        OE_INFO << LC
            << "Setting default cache policy from map ("
            << _mapOptions.cachePolicy()->usageString() << ")" << std::endl;
    }

    // the map-side dbOptions object holds I/O information for all components.
    _readOptions = osg::clone( Registry::instance()->getDefaultOptions() );

    // put the CacheSettings object in there. We will propogate this throughout
    // the data model and the renderer.
    CacheSettings* cacheSettings = new CacheSettings();

    // Set up a cache if there's one in the options:
    if (_mapOptions.cache().isSet())
        cacheSettings->setCache(CacheFactory::create(_mapOptions.cache().get()));

    // Otherwise use the registry default cache if there is one:
    if (cacheSettings->getCache() == 0L)
        cacheSettings->setCache(Registry::instance()->getDefaultCache());

    // Integrate local cache policy (which can be overridden by the environment)
    cacheSettings->integrateCachePolicy(_mapOptions.cachePolicy());

    // store in the options so we can propagate it to layers, etc.
    cacheSettings->store(_readOptions.get());

    OE_INFO << LC << cacheSettings->toString() << "\n";


    // remember the referrer for relative-path resolution:
    URIContext( _mapOptions.referrer() ).store( _readOptions.get() );

    // we do our own caching
    _readOptions->setObjectCacheHint( osgDB::Options::CACHE_NONE );

    // encode this map in the read options.
    _readOptions->getOrCreateUserDataContainer()->addUserObject(this);

    // set up a callback that the Map will use to detect Elevation Layer
    // visibility changes
    _elevationLayerCB = new ElevationLayerCB(this);

    // elevation sampling
    _elevationPool = new ElevationPool();
    _elevationPool->setMap( this );
}

Map::~Map()
{
    if (_elevationPool)
        delete _elevationPool;

    OE_DEBUG << "~Map" << std::endl;
}

ElevationPool*
Map::getElevationPool() const
{
    return _elevationPool; //.get();
}

void
Map::notifyElevationLayerVisibleChanged(TerrainLayer* layer)
{
    // bump the revision safely:
    Revision newRevision;
    {
        Threading::ScopedWriteLock lock( const_cast<Map*>(this)->_mapDataMutex );
        newRevision = ++_dataModelRevision;
    }

    // a separate block b/c we don't need the mutex
    for( MapCallbackList::iterator i = _mapCallbacks.begin(); i != _mapCallbacks.end(); i++ )
    {
        i->get()->onMapModelChanged( MapModelChange(
            MapModelChange::TOGGLE_ELEVATION_LAYER, newRevision, layer) );
    }
}

bool
Map::isGeocentric() const
{
    return
        _mapOptions.coordSysType() == MapOptions::CSTYPE_GEOCENTRIC ||
        _mapOptions.coordSysType() == MapOptions::CSTYPE_GEOCENTRIC_CUBE;
}

const osgDB::Options*
Map::getGlobalOptions() const
{
    return _globalOptions.get();
}

void
Map::setGlobalOptions( const osgDB::Options* options )
{
    _globalOptions = options;
}

void
Map::setMapName( const std::string& name ) {
    _name = name;
}

Revision
Map::getDataModelRevision() const
{
    return _dataModelRevision;
}

const Profile*
Map::getProfile() const
{
    if ( !_profile.valid() )
        const_cast<Map*>(this)->calculateProfile();
    return _profile.get();
}

Cache*
Map::getCache() const
{
    CacheSettings* cacheSettings = CacheSettings::get(_readOptions.get());
    return cacheSettings ? cacheSettings->getCache() : 0L;
}

void
Map::setCache(Cache* cache)
{
    // note- probably unsafe to do this after initializing the terrain. so don't.
    CacheSettings* cacheSettings = CacheSettings::get(_readOptions.get());
    if (cacheSettings && cacheSettings->getCache() != cache)
        cacheSettings->setCache(cache);
}

void
Map::addMapCallback( MapCallback* cb ) const
{
    if ( cb )
        const_cast<Map*>(this)->_mapCallbacks.push_back( cb );
}

void
Map::removeMapCallback( MapCallback* cb )
{
    MapCallbackList::iterator i = std::find( _mapCallbacks.begin(), _mapCallbacks.end(), cb);
    if (i != _mapCallbacks.end())
    {
        _mapCallbacks.erase( i );
    }
}

void
Map::beginUpdate()
{
    MapModelChange msg( MapModelChange::BEGIN_BATCH_UPDATE, _dataModelRevision );

    for( MapCallbackList::iterator i = _mapCallbacks.begin(); i != _mapCallbacks.end(); i++ )
    {
        i->get()->onMapModelChanged( msg );
    }
}

void
Map::endUpdate()
{
    MapModelChange msg( MapModelChange::END_BATCH_UPDATE, _dataModelRevision );

    for( MapCallbackList::iterator i = _mapCallbacks.begin(); i != _mapCallbacks.end(); i++ )
    {
        i->get()->onMapModelChanged( msg );
    }
}

void
Map::addLayer(Layer* layer)
{
    osgEarth::Registry::instance()->clearBlacklist();
    if ( layer )
    {
        TerrainLayer* terrainLayer = dynamic_cast<TerrainLayer*>(layer);
        if (terrainLayer)
        {
            // Set the DB options for the map from the layer, including the cache policy.
            terrainLayer->setReadOptions( _readOptions.get() );

            // Tell the layer the map profile, if supported:
            if ( _profile.valid() )
            {
                terrainLayer->setTargetProfileHint( _profile.get() );
            }

            // open the layer:
            terrainLayer->open();
        }

        ElevationLayer* elevationLayer = dynamic_cast<ElevationLayer*>(layer);
        if (elevationLayer)
        {
            elevationLayer->addCallback(_elevationLayerCB.get());
        }

        ModelLayer* modelLayer = dynamic_cast<ModelLayer*>(layer);
        if (modelLayer)
        {
            // initialize the model layer
            modelLayer->setReadOptions(_readOptions.get());

            // open it and check the status
            modelLayer->open();
        }

        MaskLayer* maskLayer = dynamic_cast<MaskLayer*>(layer);
        if (maskLayer)
        {
            // initialize the model layer
            maskLayer->setReadOptions(_readOptions.get());

            // open it and check the status
            maskLayer->open();
        }

        int newRevision;
        unsigned index = -1;

        // Add the layer to our stack.
        {
            Threading::ScopedWriteLock lock( _mapDataMutex );

            _layers.push_back( layer );
            index = _layers.size() - 1;
            newRevision = ++_dataModelRevision;
        }

        // a separate block b/c we don't need the mutex
        for( MapCallbackList::iterator i = _mapCallbacks.begin(); i != _mapCallbacks.end(); i++ )
        {
            i->get()->onMapModelChanged(MapModelChange(
                MapModelChange::ADD_LAYER, newRevision, layer, index));
        }
    }
}

void
Map::insertLayer(Layer* layer, unsigned index)
{
    osgEarth::Registry::instance()->clearBlacklist();
    if ( layer )
    {
        TerrainLayer* terrainLayer = dynamic_cast<TerrainLayer*>(layer);
        if (terrainLayer)
        {
            // Set the DB options for the map from the layer, including the cache policy.
            terrainLayer->setReadOptions( _readOptions.get() );

            // Tell the layer the map profile, if supported:
            if ( _profile.valid() )
            {
                terrainLayer->setTargetProfileHint( _profile.get() );
            }

            // open the layer:
            terrainLayer->open();
        }

        ModelLayer* modelLayer = dynamic_cast<ModelLayer*>(layer);
        if (modelLayer)
        {
            // initialize the model layer
            modelLayer->setReadOptions(_readOptions.get());

            // open it and check the status
            modelLayer->open();
        }

        ElevationLayer* elevationLayer = dynamic_cast<ElevationLayer*>(layer);
        if (elevationLayer)
        {
            elevationLayer->addCallback(_elevationLayerCB.get());
        }

        MaskLayer* maskLayer = dynamic_cast<MaskLayer*>(layer);
        if (maskLayer)
        {
            // initialize the model layer
            maskLayer->setReadOptions(_readOptions.get());

            // open it and check the status
            maskLayer->open();
        }

        int newRevision;

        // Add the layer to our stack.
        {
            Threading::ScopedWriteLock lock( _mapDataMutex );

            if (index >= _layers.size())
                _layers.push_back(layer);
            else
                _layers.insert( _layers.begin() + index, layer );

            newRevision = ++_dataModelRevision;
        }

        // a separate block b/c we don't need the mutex
        for( MapCallbackList::iterator i = _mapCallbacks.begin(); i != _mapCallbacks.end(); i++ )
        {
            //i->get()->onMapModelChanged( MapModelChange(
            //    MapModelChange::ADD_IMAGE_LAYER, newRevision, layer, index) );

            i->get()->onMapModelChanged(MapModelChange(
                MapModelChange::ADD_LAYER, newRevision, layer, index));
        }
    }
}

void
Map::removeLayer(Layer* layer)
{
    osgEarth::Registry::instance()->clearBlacklist();
    unsigned int index = -1;

    osg::ref_ptr<Layer> layerToRemove = layer;
    Revision newRevision;

    if ( layerToRemove.get() )
    {
        Threading::ScopedWriteLock lock( _mapDataMutex );
        index = 0;
        for( LayerVector::iterator i = _layers.begin(); i != _layers.end(); i++, index++ )
        {
            if ( i->get() == layerToRemove.get() )
            {
                _layers.erase( i );
                newRevision = ++_dataModelRevision;
                break;
            }
        }
    }

    ElevationLayer* elevationLayer = dynamic_cast<ElevationLayer*>(layerToRemove.get());
    if (elevationLayer)
    {
        elevationLayer->removeCallback(_elevationLayerCB.get());
    }

    // a separate block b/c we don't need the mutex
    if ( newRevision >= 0 ) // layerToRemove.get() )
    {
        for( MapCallbackList::iterator i = _mapCallbacks.begin(); i != _mapCallbacks.end(); i++ )
        {
            i->get()->onMapModelChanged( MapModelChange(
                MapModelChange::REMOVE_LAYER, newRevision, layerToRemove.get(), index) );
        }
    }
}

void
Map::moveLayer(Layer* layer, unsigned newIndex)
{
    unsigned int oldIndex = 0;
    unsigned int actualIndex = 0;
    Revision newRevision;

    if ( layer )
    {
        Threading::ScopedWriteLock lock( _mapDataMutex );

        // preserve the layer with a ref:
        osg::ref_ptr<Layer> layerToMove = layer;

        // find it:
        LayerVector::iterator i_oldIndex = _layers.end();
        for( LayerVector::iterator i = _layers.begin(); i != _layers.end(); i++, actualIndex++ )
        {
            if ( i->get() == layer )
            {
                i_oldIndex = i;
                oldIndex = actualIndex;
                break;
            }
        }

        if ( i_oldIndex == _layers.end() )
            return; // layer not found in list

        // erase the old one and insert the new one.
        _layers.erase( i_oldIndex );
        _layers.insert( _layers.begin() + newIndex, layerToMove.get() );

        newRevision = ++_dataModelRevision;
    }

    // a separate block b/c we don't need the mutex
    if ( layer )
    {
        for( MapCallbackList::iterator i = _mapCallbacks.begin(); i != _mapCallbacks.end(); i++ )
        {
            i->get()->onMapModelChanged( MapModelChange(
                MapModelChange::MOVE_LAYER, newRevision, layer, oldIndex, newIndex) );
        }
    }
}

Revision
Map::getLayers(LayerVector& out_list) const
{
    out_list.reserve( _layers.size() );

    Threading::ScopedReadLock lock( const_cast<Map*>(this)->_mapDataMutex );
    for( LayerVector::const_iterator i = _layers.begin(); i != _layers.end(); ++i )
        out_list.push_back( i->get() );

    return _dataModelRevision;
}

unsigned
Map::getNumLayers() const
{
    Threading::ScopedReadLock lock( const_cast<Map*>(this)->_mapDataMutex );
    return _layers.size();
}

Layer*
Map::getLayerByName(const std::string& name) const
{
    Threading::ScopedReadLock( const_cast<Map*>(this)->_mapDataMutex );
    for(LayerVector::const_iterator i = _layers.begin(); i != _layers.end(); ++i)
        if ( i->get()->getName() == name )
            return i->get();
    return 0L;
}

Layer*
Map::getLayerByUID(UID layerUID) const
{
    Threading::ScopedReadLock( const_cast<Map*>(this)->_mapDataMutex );
    for( LayerVector::const_iterator i = _layers.begin(); i != _layers.end(); ++i )
        if ( i->get()->getUID() == layerUID )
            return i->get();
    return 0L;
}

Layer*
Map::getLayerAt(unsigned index) const
{
    Threading::ScopedReadLock( const_cast<Map*>(this)->_mapDataMutex );
    if ( index >= 0 && index < (int)_layers.size() )
        return _layers[index].get();
    else
        return 0L;
}

unsigned
Map::getIndexOfLayer(Layer* layer) const
{
    Threading::ScopedReadLock( const_cast<Map*>(this)->_mapDataMutex );
    unsigned index = 0;
    for (; index < _layers.size(); ++index)
    {
        if (_layers[index] == layer)
            break;
    }
    return index;
}


void
Map::clear()
{
    LayerVector layersRemoved;

    //ImageLayerVector     imageLayersRemoved;
    //ElevationLayerVector elevLayersRemoved;
    //ModelLayerVector     modelLayersRemoved;
    //MaskLayerVector      maskLayersRemoved;

    Revision newRevision;
    {
        Threading::ScopedWriteLock lock( _mapDataMutex );

        layersRemoved.swap( _layers );
        //imageLayersRemoved.swap( _imageLayers );
        //elevLayersRemoved.swap ( _elevationLayers );
        //modelLayersRemoved.swap( _modelLayers );

        // calculate a new revision.
        newRevision = ++_dataModelRevision;
    }

    // a separate block b/c we don't need the mutex
    for( MapCallbackList::iterator i = _mapCallbacks.begin(); i != _mapCallbacks.end(); i++ )
    {
        for(LayerVector::iterator layer = layersRemoved.begin();
            layer != layersRemoved.end();
            ++layer)
        {
            i->get()->onMapModelChanged(MapModelChange(MapModelChange::REMOVE_LAYER, newRevision, layer->get()));
        }
    }
}


void
Map::setLayersFromMap(const Map* map)
{
    this->clear();

    if ( map )
    {
        LayerVector layers;
        map->getLayers(layers);
        for (LayerVector::iterator i = layers.begin(); i != layers.end(); ++i)
            addLayer(i->get());
    }
}


void
Map::calculateProfile()
{
    if ( !_profile.valid() )
    {
        osg::ref_ptr<const Profile> userProfile;
        if ( _mapOptions.profile().isSet() )
        {
            userProfile = Profile::create( _mapOptions.profile().value() );
        }

        if ( _mapOptions.coordSysType() == MapOptions::CSTYPE_GEOCENTRIC )
        {
            if ( userProfile.valid() )
            {
                if ( userProfile->isOK() && userProfile->getSRS()->isGeographic() )
                {
                    _profile = userProfile.get();
                }
                else
                {
                    OE_WARN << LC
                        << "Map is geocentric, but the configured profile SRS ("
                        << userProfile->getSRS()->getName() << ") is not geographic; "
                        << "it will be ignored."
                        << std::endl;
                }
            }
        }
        else if ( _mapOptions.coordSysType() == MapOptions::CSTYPE_GEOCENTRIC_CUBE )
        {
            if ( userProfile.valid() )
            {
                if ( userProfile->isOK() && userProfile->getSRS()->isCube() )
                {
                    _profile = userProfile.get();
                }
                else
                {
                    OE_WARN << LC
                        << "Map is geocentric cube, but the configured profile SRS ("
                        << userProfile->getSRS()->getName() << ") is not geocentric cube; "
                        << "it will be ignored."
                        << std::endl;
                }
            }
        }
        else // CSTYPE_PROJECTED
        {
            if ( userProfile.valid() )
            {
                if ( userProfile->isOK() && userProfile->getSRS()->isProjected() )
                {
                    _profile = userProfile.get();
                }
                else
                {
                    OE_WARN << LC
                        << "Map is projected, but the configured profile SRS ("
                        << userProfile->getSRS()->getName() << ") is not projected; "
                        << "it will be ignored."
                        << std::endl;
                }
            }
        }

        // At this point, if we don't have a profile we need to search tile sources until we find one.
        if ( !_profile.valid() )
        {
            Threading::ScopedReadLock lock( _mapDataMutex );

            for (LayerVector::iterator i = _layers.begin(); i != _layers.end(); ++i)
            {
                TerrainLayer* terrainLayer = dynamic_cast<TerrainLayer*>(i->get());
                if (terrainLayer && terrainLayer->getTileSource())
                {
                    _profile = terrainLayer->getTileSource()->getProfile();
                }
            }
        }

        // ensure that the profile we found is the correct kind
        // convert a geographic profile to Plate Carre if necessary
        if ( _mapOptions.coordSysType() == MapOptions::CSTYPE_GEOCENTRIC && !( _profile.valid() && _profile->getSRS()->isGeographic() ) )
        {
            // by default, set a geocentric map to use global-geodetic WGS84.
            _profile = osgEarth::Registry::instance()->getGlobalGeodeticProfile();
        }
        else if ( _mapOptions.coordSysType() == MapOptions::CSTYPE_GEOCENTRIC_CUBE && !( _profile.valid() && _profile->getSRS()->isCube() ) )
        {
            //If the map type is a Geocentric Cube, set the profile to the cube profile.
            _profile = osgEarth::Registry::instance()->getCubeProfile();
        }
        else if ( _mapOptions.coordSysType() == MapOptions::CSTYPE_PROJECTED && _profile.valid() && _profile->getSRS()->isGeographic() )
        {
            OE_INFO << LC << "Projected map with geographic SRS; activating EQC profile" << std::endl;
            unsigned u, v;
            _profile->getNumTiles(0, u, v);
            const osgEarth::SpatialReference* eqc = _profile->getSRS()->createEquirectangularSRS();
            osgEarth::GeoExtent e = _profile->getExtent().transform( eqc );
            _profile = osgEarth::Profile::create( eqc, e.xMin(), e.yMin(), e.xMax(), e.yMax(), u, v);
        }
        else if ( _mapOptions.coordSysType() == MapOptions::CSTYPE_PROJECTED && !( _profile.valid() && _profile->getSRS()->isProjected() ) )
        {
            // TODO: should there be a default projected profile?
            _profile = 0;
        }

        // finally, fire an event if the profile has been set.
        if ( _profile.valid() )
        {
            OE_INFO << LC << "Map profile is: " << _profile->toString() << std::endl;

            for( MapCallbackList::iterator i = _mapCallbacks.begin(); i != _mapCallbacks.end(); i++ )
            {
                i->get()->onMapInfoEstablished( MapInfo(this) );
            }
        }

        else
        {
            OE_WARN << LC << "Warning, not yet able to establish a map profile!" << std::endl;
        }
    }

    if ( _profile.valid() )
    {
        // tell all the loaded layers what the profile is, as a hint
        {
            Threading::ScopedWriteLock lock( _mapDataMutex );

            for (LayerVector::iterator i = _layers.begin(); i != _layers.end(); ++i)
            {
                TerrainLayer* terrainLayer = dynamic_cast<TerrainLayer*>(i->get());
                if (terrainLayer && terrainLayer->getEnabled())
                {
                    terrainLayer->setTargetProfileHint(_profile.get());
                }
            }
        }

        // create a "proxy" profile to use when querying elevation layers with a vertical datum
        if ( _profile->getSRS()->getVerticalDatum() != 0L )
        {
            ProfileOptions po = _profile->toProfileOptions();
            po.vsrsString().unset();
            _profileNoVDatum = Profile::create(po);
        }
        else
        {
            _profileNoVDatum = _profile;
        }
    }
}

const SpatialReference*
Map::getWorldSRS() const
{
    return isGeocentric() ? getSRS()->getECEF() : getSRS();
}

bool
Map::sync( MapFrame& frame ) const
{
    bool result = false;

    if ( frame._mapDataModelRevision != _dataModelRevision || !frame._initialized )
    {
        // hold the read lock while copying the layer lists.
        Threading::ScopedReadLock lock( const_cast<Map*>(this)->_mapDataMutex );

        if (!frame._initialized)
            frame._layers.reserve(_layers.size());

        frame._layers.clear();

        std::copy(_layers.begin(), _layers.end(), std::back_inserter(frame._layers));

        // sync the revision numbers.
        frame._initialized = true;
        frame._mapDataModelRevision = _dataModelRevision;

        result = true;
    }
    return result;
}