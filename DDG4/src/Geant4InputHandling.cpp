// $Id: Geant4Field.cpp 888 2013-11-14 15:54:56Z markus.frank@cern.ch $
//====================================================================
//  AIDA Detector description implementation for LCD
//--------------------------------------------------------------------
//
//  Author     : M.Frank
//
//====================================================================
// Framework include files
#include "DDG4/Geant4InputHandling.h"
#include "DDG4/Geant4Primary.h"
#include "DDG4/Geant4Context.h"
#include "DDG4/Geant4Action.h"
#include "CLHEP/Units/SystemOfUnits.h"
#include "CLHEP/Units/PhysicalConstants.h"

#include "G4ParticleDefinition.hh"
#include "G4Event.hh"
#include "G4PrimaryVertex.hh"
#include "G4PrimaryParticle.hh"

// C/C++ include files
#include <stdexcept>
#include <cmath>

using namespace std;
using namespace DD4hep;
using namespace DD4hep::Simulation;

typedef ReferenceBitMask<int> PropertyMask;

/// Initialize the generation of one event
int DD4hep::Simulation::generationInitialization(const Geant4Action* /* caller */,
						 const Geant4Context* context)
{
  /**
   *  This action should register all event extension required for the further 
   *  processing. We want to avoid that every client has to check if a given 
   *  object is present or not and than later install the required data structures.
   */
  context->event().addExtension(new Geant4PrimaryMap());

  // The final set of created particles in the simulation.
  context->event().addExtension(new Geant4ParticleMap());

  //
  // The Geant4PrimaryEvent extension contains a whole set of 
  // Geant4PrimaryInteraction objects each may represent a complete
  // interaction. Particles and vertices may be unbiased.
  // This is the input to the translator forming the final 
  // Geant4PrimaryInteraction (see below) containing rebiased particle
  // and vertex maps.
  Geant4PrimaryEvent* evt = new Geant4PrimaryEvent();
  context->event().addExtension(evt);
  //
  // The Geant4PrimaryInteraction extension contains the final
  // vertices and particles ready to be injected to Geant4.
  Geant4PrimaryInteraction* inter = new Geant4PrimaryInteraction();
  inter->setNextPID(-1);
  context->event().addExtension(inter);
  return 1;
}

/// Append input interaction to global output
static void appendInteraction(const Geant4Action* caller,
			      Geant4PrimaryInteraction* output, 
			      Geant4PrimaryInteraction* input)
{
  Geant4PrimaryInteraction::ParticleMap::iterator ip, ipend;
  for( ip=input->particles.begin(), ipend=input->particles.end(); ip != ipend; ++ip )  {
    Geant4Particle* p = (*ip).second;
    output->particles.insert(make_pair(p->id,p->addRef()));
  }
  Geant4PrimaryInteraction::VertexMap::iterator ivfnd, iv, ivend;
  for( iv=input->vertices.begin(), ivend=input->vertices.end(); iv != ivend; ++iv )   {
    ivfnd = output->vertices.find((*iv).second->mask);
    if ( ivfnd != output->vertices.end() )   {
      caller->abortRun("Duplicate primary interaction identifier!",
		       "Cannot handle 2 interactions with identical identifiers!");
    }
    output->vertices.insert(make_pair((*iv).second->mask,(*iv).second->addRef()));
  }
}

static void rebaseParticles(Geant4PrimaryInteraction::ParticleMap& particles, int &offset)    {
  Geant4PrimaryInteraction::ParticleMap::iterator ip, ipend;
  int mx_id = offset;
  // Now move begin and end-vertex of all primary and generator particles accordingly
  for( ip=particles.begin(), ipend=particles.end(); ip != ipend; ++ip )  {
    Geant4ParticleHandle p((*ip).second);
    p.offset(offset);
    mx_id = p->id+1 > mx_id ? p->id+1 : mx_id;
  }
  offset = mx_id;
}

static void rebaseVertices(Geant4PrimaryInteraction::VertexMap& vertices, int part_offset)    {
  Geant4PrimaryInteraction::VertexMap::iterator iv, ivend;    
  set<int> in, out;
  set<int>::iterator i;
  // Now move begin and end-vertex of all primary vertices accordingly
  for(iv=vertices.begin(), ivend=vertices.end(); iv != ivend; ++iv)  {
    Geant4Vertex* v = (*iv).second;
    in  = v->in;
    out = v->out;
    for(in=v->in, i=in.begin(), v->in.clear(); i != in.end(); ++i)
      v->in.insert((*i)+part_offset);
    for(out=v->out, i=out.begin(), v->out.clear(); i != out.end(); ++i)
      v->out.insert((*i)+part_offset);
  }
}

/// Merge all interactions present in the context
int DD4hep::Simulation::mergeInteractions(const Geant4Action* caller,
					  const Geant4Context* context)   
{
  typedef Geant4PrimaryEvent::Interaction  Interaction;
  typedef vector<Interaction*>             Interactions;
  Geant4Event& event = context->event();
  Geant4PrimaryEvent* evt = event.extension<Geant4PrimaryEvent>();
  Interaction* output = event.extension<Interaction>();
  Interactions inter  = evt->interactions();
  int particle_offset = 0;

  for(Interactions::const_iterator i=inter.begin(); i != inter.end(); ++i)  {
    Interaction* interaction = *i;
    int vertex_offset = particle_offset;
    if ( !interaction->applyMask() )   {
      caller->abortRun("Found single interaction with multiple primary vertices!",
		       "Cannot merge individual interactions with more than one primary!");
    }
    rebaseParticles(interaction->particles,particle_offset);
    rebaseVertices(interaction->vertices,vertex_offset);
    appendInteraction(caller,output,interaction);
  }
  output->setNextPID(particle_offset);
  Geant4PrimaryInteraction::ParticleMap::iterator ip, ipend;
  caller->debug("+++ Merging MC input record from %d interactions:",(int)inter.size());
  for( ip=output->particles.begin(), ipend=output->particles.end(); ip != ipend; ++ip )
    Geant4ParticleHandle((*ip).second).dump1(DEBUG,caller->name(),"Merged particles");
  return 1;
}

/// Boost particles of one interaction identified by its mask
int DD4hep::Simulation::boostInteraction(const Geant4Action* /* caller */,
					 Geant4PrimaryEvent::Interaction* inter, 
					 double alpha)
{
#define SQR(x)  (x*x)
  Geant4PrimaryEvent::Interaction::VertexMap::iterator iv;    
  Geant4PrimaryEvent::Interaction::ParticleMap::iterator ip;
  double gamma = std::sqrt(1 + SQR(tan(alpha)));
  double betagamma = std::tan(alpha);

  if ( alpha != 0.0 )  {
    // Now move begin and end-vertex of all primary vertices accordingly
    for(iv=inter->vertices.begin(); iv != inter->vertices.end(); ++iv)  {
      Geant4Vertex* v = (*iv).second;
      double t = gamma * v->time + betagamma * v->x / c_light;
      double x = gamma * v->x + betagamma * c_light * v->time;
      double y = v->y;
      double z = v->z;
      v->x = x;
      v->y = y;
      v->z = z;
      v->time = t;
    }

    // Now move begin and end-vertex of all primary and generator particles accordingly
    for(ip=inter->particles.begin(); ip != inter->particles.end(); ++ip)  {
      Geant4Particle* p = (*ip).second;
      double t = gamma * p->time + betagamma * p->vsx / c_light;
      double x = gamma * p->vsx + betagamma * c_light * p->time;
      double y = p->vsx;
      double z = p->vsz;

      double m  = p->definition->GetPDGMass();
      double e2 = SQR(p->psx)+SQR(p->psy)+SQR(p->psz)+SQR(m);
      double px = betagamma * std::sqrt(e2) + gamma * p->psx;
      double py = p->psy;
      double pz = p->psz;

      p->vsx = x;
      p->vsy = y;
      p->vsz = z;
      p->time = t;

      p->psx += px;
      p->psy += py;
      p->psz += pz;
    }
  }
  return 1;
}

/// Smear the primary vertex of an interaction
int DD4hep::Simulation::smearInteraction(const Geant4Action* /* caller */,
					 Geant4PrimaryEvent::Interaction* inter, 
					 double dx, double dy, double dz, double dt)
{
  Geant4PrimaryEvent::Interaction::VertexMap::iterator iv;    
  Geant4PrimaryEvent::Interaction::ParticleMap::iterator ip;

  // Now move begin and end-vertex of all primary vertices accordingly
  for(iv=inter->vertices.begin(); iv != inter->vertices.end(); ++iv)  {
    Geant4Vertex* v = (*iv).second;
    v->x += dx;
    v->y += dy;
    v->z += dz;
    v->time += dt;
  }
  // Now move begin and end-vertex of all primary and generator particles accordingly
  for(ip=inter->particles.begin(); ip != inter->particles.end(); ++ip)  {
    Geant4Particle* p = (*ip).second;
    p->vsx += dx;
    p->vsy += dy;
    p->vsz += dz;
    p->vex += dx;
    p->vey += dy;
    p->vez += dz;
    p->time += dt;
  }
  return 1;
}

static G4PrimaryParticle* createG4Primary(const Geant4ParticleHandle p)  {
  G4PrimaryParticle* g4 = new G4PrimaryParticle(p->pdgID, p->psx, p->psy, p->psz);
  g4->SetMass(p->mass);
  return g4;
}

static map<Geant4Particle*,G4PrimaryParticle*>
getRelevant(set<int>& visited,
	    map<int,G4PrimaryParticle*>& prim,
	    Geant4PrimaryInteraction::ParticleMap& pm, 
	    const Geant4ParticleHandle p)  
{
  typedef map<Geant4Particle*,G4PrimaryParticle*> Primaries;
  Primaries res;
  visited.insert(p->id);
  PropertyMask status(p->status);
  if ( status.isSet(G4PARTICLE_GEN_STABLE) )  {
    if ( prim.find(p->id) == prim.end() )  {
      G4PrimaryParticle* p4 = createG4Primary(p);
      prim[p->id] = p4;
      res.insert(make_pair(p,p4));
    }
  }
  else if ( p->daughters.size() > 0 )  {
    const Geant4Particle::Particles& dau = p->daughters;
    int first_daughter = *(dau.begin());
    Geant4ParticleHandle dp = pm[first_daughter];
    double en = p.energy();
    double me = en > std::numeric_limits<double>::epsilon() ? p->mass / en : 0.0;
    //  fix by S.Morozov for real != 0
    double proper_time = fabs(dp->time-p->time) * me;
    double proper_time_Precision =  pow(10.,-DBL_DIG)*me*fmax(fabs(p->time),fabs(dp->time));
    bool isProperTimeZero = ( proper_time <= proper_time_Precision ) ;
    // -- remove original --- if (proper_time != 0) {
    if ( !isProperTimeZero ) {
      map<int,G4PrimaryParticle*>::iterator ip4 = prim.find(p->id);
      G4PrimaryParticle* p4 = (ip4 == prim.end()) ? 0 : (*ip4).second;
      if ( !p4 )  {
	p4 = createG4Primary(p);
	p4->SetProperTime(proper_time);
	prim[p->id] = p4;
	Primaries daughters;
	for(Geant4Particle::Particles::const_iterator i=dau.begin(); i!=dau.end(); ++i)  {
	  if ( visited.find(*i) == visited.end() )  {
	    Primaries tmp = getRelevant(visited,prim,pm,pm[*i]);
	    daughters.insert(tmp.begin(),tmp.end());
	  }
	}
	for(Primaries::iterator i=daughters.begin(); i!=daughters.end(); ++i)
	  p4->SetDaughter((*i).second);
      }
      res.insert(make_pair(p,p4));
    }
    else  {
      for(Geant4Particle::Particles::const_iterator i=dau.begin(); i!=dau.end(); ++i)  {
	if ( visited.find(*i) == visited.end() )  {
	  Primaries tmp = getRelevant(visited,prim,pm,pm[*i]);
	  res.insert(tmp.begin(),tmp.end());
	}
      }
    }
  }
  return res;
}

/// Generate all primary vertices corresponding to the merged interaction
int DD4hep::Simulation::generatePrimaries(const Geant4Action* caller,
					  const Geant4Context* context,
					  G4Event* event)
{
  typedef map<Geant4Particle*,G4PrimaryParticle*> Primaries;
  typedef Geant4PrimaryInteraction Interaction;
  Geant4PrimaryMap* primaries   = context->event().extension<Geant4PrimaryMap>();
  Interaction*      interaction = context->event().extension<Interaction>();
  Interaction::ParticleMap& pm  = interaction->particles;
  Interaction::VertexMap&   vm  = interaction->vertices;
  map<int,G4PrimaryParticle*> prim;
  set<int> visited;

  Geant4PrimaryInteraction::VertexMap::iterator ivfnd, iv, ivend;
  for(Interaction::VertexMap::const_iterator iend=vm.end(),i=vm.begin(); i!=iend; ++i)  {
    int num_part = 0;
    Geant4Vertex* v = (*i).second;
    G4PrimaryVertex* v4 = new G4PrimaryVertex(v->x,v->y,v->z,v->time);
    event->AddPrimaryVertex(v4);
    caller->print("+++++ G4PrimaryVertex at (%+.2e,%+.2e,%+.2e) [mm] %+.2e [ns]",
		  v->x/mm,v->y/mm,v->z/mm,v->time/ns);    
    for(Geant4Vertex::Particles::const_iterator ip=v->out.begin(); ip!=v->out.end(); ++ip)  {
      Geant4ParticleHandle p = pm[*ip];
      if ( p->parents.size() == 0 )  {
	Primaries relevant = getRelevant(visited,prim,pm,p);
	for(Primaries::const_iterator j=relevant.begin(); j!= relevant.end(); ++j)  {
	  Geant4ParticleHandle r = (*j).first;
	  G4PrimaryParticle* p4 = (*j).second;
	  PropertyMask reason(r->reason);
	  reason.set(G4PARTICLE_PRIMARY);
	  v4->SetPrimary(p4);
	  caller->printM1("+++ +-> G4Primary[%3d] ID:%3d type:%9d/%-12s "
			  "Momentum:(%+.2e,%+.2e,%+.2e) [GeV] time:%+.2e [ns] #Par:%3d #Dau:%3d",
			  num_part,r->id,r->pdgID,r.particleName().c_str(),
			  r->psx/GeV,r->psy/GeV,r->psz/GeV,r->time/ns,
			  int(r->parents.size()),int(r->daughters.size()));
	  ++num_part;
	}
      }
    }
  }
  for(map<int,G4PrimaryParticle*>::iterator i=prim.begin(); i!=prim.end(); ++i)  {
    Geant4ParticleHandle p = pm[(*i).first];
    primaries->primaryMap[(*i).second] = p->addRef();
  }
  return 1;
}