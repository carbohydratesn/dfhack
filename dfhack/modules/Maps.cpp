/*
www.sourceforge.net/projects/dfhack
Copyright (c) 2009 Petr Mrázek (peterix), Kenneth Ferland (Impaler[WrG]), dorf

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

#include "DFCommonInternal.h"
#include <shms.h>
#include <mod-core.h>
#include <mod-maps.h>
#include "../private/APIPrivate.h"
#include "modules/Maps.h"
#include "DFError.h"
#include "DFMemInfo.h"
#include "DFProcess.h"
#include "DFVector.h"

#define SHMMAPSHDR ((Server::Maps::shm_maps_hdr *)d->d->shm_start)
#define SHMCMD(num) ((shm_cmd *)d->d->shm_start)[num]->pingpong
#define SHMHDR ((shm_core_hdr *)d->d->shm_start)
#define SHMDATA(type) ((type *)(d->d->shm_start + SHM_HEADER))

using namespace DFHack;

struct Maps::Private
{
    uint32_t * block;
    uint32_t x_block_count, y_block_count, z_block_count;
    uint32_t regionX, regionY, regionZ;
    uint32_t worldSizeX, worldSizeY;

    uint32_t maps_module;
    Server::Maps::maps_offsets offsets;
    
    APIPrivate *d;
    bool Inited;
    bool Started;
    //uint32_t biome_stuffs;
    //vector<uint16_t> v_geology[eBiomeCount];
};

Maps::Maps(APIPrivate* _d)
{
    d = new Private;
    d->d = _d;
    d->Inited = d->Started = false;
    
    DFHack::memory_info * mem = d->d->offset_descriptor;
    Server::Maps::maps_offsets &off = d->offsets;
    
    // get the offsets once here
    off.map_offset = mem->getAddress ("map_data");
    off.x_count_offset = mem->getAddress ("x_count_block");
    off.y_count_offset = mem->getAddress ("y_count_block");
    off.z_count_offset = mem->getAddress ("z_count_block");
    off.tile_type_offset = mem->getOffset ("type");
    off.designation_offset = mem->getOffset ("designation");
    //d->biome_stuffs = d->offset_descriptor->getOffset ("biome_stuffs");
    off.veinvector = mem->getOffset ("v_vein");
    
    // these can fail and will be found when looking at the actual veins later
    // basically a cache
    off.vein_ice_vptr = 0;
    mem->resolveClassnameToVPtr("block_square_event_frozen_liquid", off.vein_ice_vptr);
    off.vein_mineral_vptr = 0;
    mem->resolveClassnameToVPtr("block_square_event_mineral",off.vein_mineral_vptr);

    // upload offsets to SHM server if possible
    d->maps_module = 0;
    if(g_pProcess->getModuleIndex("Maps2010",1,d->maps_module))
    {
        // supply the module with offsets so it can work with them
        Server::Maps::maps_offsets *off2 = SHMDATA(Server::Maps::maps_offsets);
        memcpy(off2, &(d->offsets), sizeof(Server::Maps::maps_offsets));
        full_barrier
        const uint32_t cmd = Server::Maps::MAP_INIT + (d->maps_module << 16);
        g_pProcess->SetAndWait(cmd);
    }
    d->Inited = true;
}

Maps::~Maps()
{
    if(d->Started)
        Finish();
}

/*-----------------------------------*
 *  Init the mapblock pointer array  *
 *-----------------------------------*/
bool Maps::Start()
{
    if(!d->Inited)
        return false;
    if(d->Started)
        Finish();
    Server::Maps::maps_offsets &off = d->offsets;
    // get the map pointer
    uint32_t x_array_loc = g_pProcess->readDWord (off.map_offset);
    if (!x_array_loc)
    {
        return false;
    }
    
    // get the size
    uint32_t mx, my, mz;
    mx = d->x_block_count = g_pProcess->readDWord (off.x_count_offset);
    my = d->y_block_count = g_pProcess->readDWord (off.y_count_offset);
    mz = d->z_block_count = g_pProcess->readDWord (off.z_count_offset);

    // test for wrong map dimensions
    if (mx == 0 || mx > 48 || my == 0 || my > 48 || mz == 0)
    {
        throw Error::BadMapDimensions(mx, my);
        //return false;
    }

    // alloc array for pointers to all blocks
    d->block = new uint32_t[mx*my*mz];
    uint32_t *temp_x = new uint32_t[mx];
    uint32_t *temp_y = new uint32_t[my];
    uint32_t *temp_z = new uint32_t[mz];

    g_pProcess->read (x_array_loc, mx * sizeof (uint32_t), (uint8_t *) temp_x);
    for (uint32_t x = 0; x < mx; x++)
    {
        g_pProcess->read (temp_x[x], my * sizeof (uint32_t), (uint8_t *) temp_y);
        // y -> map column
        for (uint32_t y = 0; y < my; y++)
        {
            g_pProcess->read (temp_y[y],
                   mz * sizeof (uint32_t),
                   (uint8_t *) (d->block + x*my*mz + y*mz));
        }
    }
    delete [] temp_x;
    delete [] temp_y;
    delete [] temp_z;
    return true;
}

bool Maps::Finish()
{
    if (d->block != NULL)
    {
        delete [] d->block;
        d->block = NULL;
    }
    return true;
}

bool Maps::isValidBlock (uint32_t x, uint32_t y, uint32_t z)
{
    if ( x >= d->x_block_count || y >= d->y_block_count || z >= d->z_block_count)
        return false;
    return d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z] != 0;
}

uint32_t Maps::getBlockPtr (uint32_t x, uint32_t y, uint32_t z)
{
    if ( x >= d->x_block_count || y >= d->y_block_count || z >= d->z_block_count)
        return 0;
    return d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
}

bool Maps::ReadBlock40d(uint32_t x, uint32_t y, uint32_t z, mapblock40d * buffer)
{
    if(d->d->shm_start && d->maps_module) // ACCELERATE!
    {
        SHMMAPSHDR->x = x;
        SHMMAPSHDR->y = y;
        SHMMAPSHDR->z = z;
        volatile uint32_t cmd = Server::Maps::MAP_READ_BLOCK_BY_COORDS + (d->maps_module << 16);
        if(!g_pProcess->SetAndWait(cmd))
            return false;
        memcpy(buffer,SHMDATA(mapblock40d),sizeof(mapblock40d));
        return true;
    }
    else // plain old block read
    {
        uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
        if (addr)
        {
            g_pProcess->read (addr + d->offsets.tile_type_offset, sizeof (buffer->tiletypes), (uint8_t *) buffer->tiletypes);
            buffer->origin = addr;
            uint32_t addr_of_struct = g_pProcess->readDWord(addr);
            buffer->blockflags.whole = g_pProcess->readDWord(addr_of_struct);
            return true;
        }
        return false;
    }
}


// 256 * sizeof(uint16_t)
bool Maps::ReadTileTypes (uint32_t x, uint32_t y, uint32_t z, tiletypes40d *buffer)
{
    uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
    if (addr)
    {
        g_pProcess->read (addr + d->offsets.tile_type_offset, sizeof (tiletypes40d), (uint8_t *) buffer);
        return true;
    }
    return false;
}

bool Maps::ReadDirtyBit(uint32_t x, uint32_t y, uint32_t z, bool &dirtybit)
{
    uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
    if(addr)
    {
        uint32_t addr_of_struct = g_pProcess->readDWord(addr);
        dirtybit = g_pProcess->readDWord(addr_of_struct) & 1;
        return true;
    }
    return false;
}

bool Maps::WriteDirtyBit(uint32_t x, uint32_t y, uint32_t z, bool dirtybit)
{
    uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
    if (addr)
    {
        uint32_t addr_of_struct = g_pProcess->readDWord(addr);
        uint32_t dirtydword = g_pProcess->readDWord(addr_of_struct);
        dirtydword &= 0xFFFFFFFE;
        dirtydword |= (uint32_t) dirtybit;
        g_pProcess->writeDWord (addr_of_struct, dirtydword);
        return true;
    }
    return false;
}

/// read/write the block flags
bool Maps::ReadBlockFlags(uint32_t x, uint32_t y, uint32_t z, t_blockflags &blockflags)
{
    uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
    if(addr)
    {
        uint32_t addr_of_struct = g_pProcess->readDWord(addr);
        blockflags.whole = g_pProcess->readDWord(addr_of_struct);
        return true;
    }
    return false;
}
bool Maps::WriteBlockFlags(uint32_t x, uint32_t y, uint32_t z, t_blockflags blockflags)
{
    uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
    if (addr)
    {
        uint32_t addr_of_struct = g_pProcess->readDWord(addr);
        g_pProcess->writeDWord (addr_of_struct, blockflags.whole);
        return true;
    }
    return false;
}

bool Maps::ReadDesignations (uint32_t x, uint32_t y, uint32_t z, designations40d *buffer)
{
    uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
    if (addr)
    {
        g_pProcess->read (addr + d->offsets.designation_offset, sizeof (designations40d), (uint8_t *) buffer);
        return true;
    }
    return false;
}

// 256 * sizeof(uint16_t)
bool Maps::WriteTileTypes (uint32_t x, uint32_t y, uint32_t z, tiletypes40d *buffer)
{
    uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
    if (addr)
    {
        g_pProcess->write (addr + d->offsets.tile_type_offset, sizeof (tiletypes40d), (uint8_t *) buffer);
        return true;
    }
    return false;
}


// 256 * sizeof(uint32_t)
bool Maps::WriteDesignations (uint32_t x, uint32_t y, uint32_t z, designations40d *buffer)
{
    uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
    if (addr)
    {
        g_pProcess->write (addr + d->offsets.designation_offset, sizeof (designations40d), (uint8_t *) buffer);
        return true;
    }
    return false;
}

// FIXME: this is bad. determine the real size!
//16 of them? IDK... there's probably just 7. Reading more doesn't cause errors as it's an array nested inside a block
// 16 * sizeof(uint8_t)
/*
bool Maps::ReadRegionOffsets (uint32_t x, uint32_t y, uint32_t z, biome_indices40d *buffer)
{
    uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
    if (addr)
    {
        g_pProcess->read (addr + d->biome_stuffs, sizeof (biome_indices40d), (uint8_t *) buffer);
        return true;
    }
    return false;
}
*/

// veins of a block, expects empty vein vectors
bool Maps::ReadVeins(uint32_t x, uint32_t y, uint32_t z, vector <t_vein> & veins, vector <t_frozenliquidvein>& ices)
{
    uint32_t addr = d->block[x*d->y_block_count*d->z_block_count + y*d->z_block_count + z];
    veins.clear();
    ices.clear();
    Server::Maps::maps_offsets &off = d->offsets;
    if (addr && off.veinvector)
    {
        // veins are stored as a vector of pointers to veins
        /*pointer is 4 bytes! we work with a 32bit program here, no matter what architecture we compile khazad for*/
        DfVector p_veins (d->d->p, addr + off.veinvector, 4);
        uint32_t size = p_veins.getSize();
        veins.reserve (size);

        // read all veins
        for (uint32_t i = 0; i < size;i++)
        {
            t_vein v;
            t_frozenliquidvein fv;

            // read the vein pointer from the vector
            uint32_t temp = * (uint32_t *) p_veins[i];
            uint32_t type = g_pProcess->readDWord(temp);
try_again:
            if(type == off.vein_mineral_vptr)
            {
                // read the vein data (dereference pointer)
                g_pProcess->read (temp, sizeof(t_vein), (uint8_t *) &v);
                v.address_of = temp;
                // store it in the vector
                veins.push_back (v);
            }
            else if(type == off.vein_ice_vptr)
            {
                // read the ice vein data (dereference pointer)
                g_pProcess->read (temp, sizeof(t_frozenliquidvein), (uint8_t *) &fv);
                // store it in the vector
                ices.push_back (fv);
            }
            else if(g_pProcess->readClassName(type) == "block_square_event_frozen_liquid")
            {
                off.vein_ice_vptr = type;
                goto try_again;
            }
            else if(g_pProcess->readClassName(type) == "block_square_event_mineral")
            {
                off.vein_mineral_vptr = type;
                goto try_again;
            }
        }
        return true;
    }
    return false;
}


// getter for map size
void Maps::getSize (uint32_t& x, uint32_t& y, uint32_t& z)
{
    x = d->x_block_count;
    y = d->y_block_count;
    z = d->z_block_count;
}

/*
//vector<uint16_t> v_geology[eBiomeCount];
bool API::ReadGeology (vector < vector <uint16_t> >& assign)
{
    memory_info * minfo = d->offset_descriptor;
    // get needed addresses and offsets. Now this is what I call crazy.
    int region_x_offset = minfo->getAddress ("region_x");
    int region_y_offset = minfo->getAddress ("region_y");
    int region_z_offset =  minfo->getAddress ("region_z");
    int world_offset =  minfo->getAddress ("world");
    int world_regions_offset =  minfo->getOffset ("w_regions_arr");
    int region_size =  minfo->getHexValue ("region_size");
    int region_geo_index_offset =  minfo->getOffset ("region_geo_index_off");
    int world_geoblocks_offset =  minfo->getOffset ("w_geoblocks");
    int world_size_x = minfo->getOffset ("world_size_x");
    int world_size_y = minfo->getOffset ("world_size_y");
    int geolayer_geoblock_offset = minfo->getOffset ("geolayer_geoblock_offset");

    uint32_t regionX, regionY, regionZ;
    uint16_t worldSizeX, worldSizeY;

    // read position of the region inside DF world
    g_pProcess->readDWord (region_x_offset, regionX);
    g_pProcess->readDWord (region_y_offset, regionY);
    g_pProcess->readDWord (region_z_offset, regionZ);

    // get world size
    g_pProcess->readWord (world_offset + world_size_x, worldSizeX);
    g_pProcess->readWord (world_offset + world_size_y, worldSizeY);

    // get pointer to first part of 2d array of regions
    uint32_t regions = g_pProcess->readDWord (world_offset + world_regions_offset);

    // read the geoblock vector
    DfVector geoblocks (d->p, world_offset + world_geoblocks_offset, 4);

    // iterate over 8 surrounding regions + local region
    for (int i = eNorthWest; i < eBiomeCount; i++)
    {
        // check bounds, fix them if needed
        int bioRX = regionX / 16 + (i % 3) - 1;
        if (bioRX < 0) bioRX = 0;
        if (bioRX >= worldSizeX) bioRX = worldSizeX - 1;
        int bioRY = regionY / 16 + (i / 3) - 1;
        if (bioRY < 0) bioRY = 0;
        if (bioRY >= worldSizeY) bioRY = worldSizeY - 1;

        // get pointer to column of regions
        uint32_t geoX;
        g_pProcess->readDWord (regions + bioRX*4, geoX);

        // get index into geoblock vector
        uint16_t geoindex;
        g_pProcess->readWord (geoX + bioRY*region_size + region_geo_index_offset, geoindex);

        // get the geoblock from the geoblock vector using the geoindex
        // read the matgloss pointer from the vector into temp
        uint32_t geoblock_off = * (uint32_t *) geoblocks[geoindex];

        // get the vector with pointer to layers
        DfVector geolayers (d->p, geoblock_off + geolayer_geoblock_offset , 4); // let's hope
        // make sure we don't load crap
        assert (geolayers.getSize() > 0 && geolayers.getSize() <= 16);

        d->v_geology[i].reserve (geolayers.getSize());
        // finally, read the layer matgloss
        for (uint32_t j = 0;j < geolayers.getSize();j++)
        {
            // read pointer to a layer
            uint32_t geol_offset = * (uint32_t *) geolayers[j];
            // read word at pointer + 2, store in our geology vectors
            d->v_geology[i].push_back (g_pProcess->readWord (geol_offset + 2));
        }
    }
    assign.clear();
    assign.reserve (eBiomeCount);
//     // TODO: clean this up
    for (int i = 0; i < eBiomeCount;i++)
    {
        assign.push_back (d->v_geology[i]);
    }
    return true;
}
*/