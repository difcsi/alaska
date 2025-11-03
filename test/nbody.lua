-- N-Body Gravitational Simulation
-- Usage: lua nbody.lua [num_bodies] [timestep] [num_steps]

local function parse_args()
  local n_bodies = tonumber(arg[1]) or 200
  local timestep = tonumber(arg[2]) or 0.01
  local num_steps = tonumber(arg[3]) or 1000
  return n_bodies, timestep, num_steps
end

local function create_body(x, y, z, vx, vy, vz, mass)
  return {
    pos = {x, y, z},
    vel = {vx, vy, vz},
    acc = {0, 0, 0},
    mass = mass
  }
end

local function init_bodies(n)
  local bodies = {}
  math.randomseed(os.time())
  
  for i = 1, n do
    local x = (math.random() - 0.5) * 200
    local y = (math.random() - 0.5) * 200
    local z = (math.random() - 0.5) * 200
    local vx = (math.random() - 0.5) * 2
    local vy = (math.random() - 0.5) * 2
    local vz = (math.random() - 0.5) * 2
    local mass = 1 + math.random() * 4
    
    table.insert(bodies, create_body(x, y, z, vx, vy, vz, mass))
  end
  
  return bodies
end

local function compute_forces(bodies)
  local G = 6.674e-11
  local n = #bodies
  
  -- Reset accelerations
  for i = 1, n do
    bodies[i].acc[1] = 0
    bodies[i].acc[2] = 0
    bodies[i].acc[3] = 0
  end
  
  -- Pairwise force computation
  for i = 1, n do
    for j = i + 1, n do
      local bi = bodies[i]
      local bj = bodies[j]
      
      local dx = bj.pos[1] - bi.pos[1]
      local dy = bj.pos[2] - bi.pos[2]
      local dz = bj.pos[3] - bi.pos[3]
      
      local dist_sq = dx * dx + dy * dy + dz * dz
      local dist = math.sqrt(dist_sq)
      
      -- Avoid singularity
      if dist > 0.1 then
        local f = G * bi.mass * bj.mass / dist_sq
        local f_norm = f / dist
        
        -- Force on i due to j
        bi.acc[1] = bi.acc[1] + f_norm * dx / bi.mass
        bi.acc[2] = bi.acc[2] + f_norm * dy / bi.mass
        bi.acc[3] = bi.acc[3] + f_norm * dz / bi.mass
        
        -- Force on j due to i (Newton's 3rd law)
        bj.acc[1] = bj.acc[1] - f_norm * dx / bj.mass
        bj.acc[2] = bj.acc[2] - f_norm * dy / bj.mass
        bj.acc[3] = bj.acc[3] - f_norm * dz / bj.mass
      end
    end
  end
end

local function integrate_euler(bodies, dt)
  for i = 1, #bodies do
    local b = bodies[i]
    -- Update velocity
    b.vel[1] = b.vel[1] + b.acc[1] * dt
    b.vel[2] = b.vel[2] + b.acc[2] * dt
    b.vel[3] = b.vel[3] + b.acc[3] * dt
    
    -- Update position
    b.pos[1] = b.pos[1] + b.vel[1] * dt
    b.pos[2] = b.pos[2] + b.vel[2] * dt
    b.pos[3] = b.pos[3] + b.vel[3] * dt
  end
end

local function compute_total_energy(bodies)
  local G = 6.674e-11
  local ke = 0
  local pe = 0
  
  for i = 1, #bodies do
    local b = bodies[i]
    local v_sq = b.vel[1]^2 + b.vel[2]^2 + b.vel[3]^2
    ke = ke + 0.5 * b.mass * v_sq
  end
  
  for i = 1, #bodies do
    for j = i + 1, #bodies do
      local bi = bodies[i]
      local bj = bodies[j]
      local dx = bj.pos[1] - bi.pos[1]
      local dy = bj.pos[2] - bi.pos[2]
      local dz = bj.pos[3] - bi.pos[3]
      local dist = math.sqrt(dx * dx + dy * dy + dz * dz)
      
      if dist > 0.1 then
        pe = pe - G * bi.mass * bj.mass / dist
      end
    end
  end
  
  return ke, pe, ke + pe
end

local function simulate(bodies, timestep, num_steps)
  print(string.format("Simulating %d bodies for %d steps (dt=%.4f)", 
    #bodies, num_steps, timestep))
  print("Step\tKE\t\tPE\t\tTotal E")
  
  for step = 1, num_steps do
    compute_forces(bodies)
    integrate_euler(bodies, timestep)
    
    if step % (num_steps / 10) == 0 or step == 1 then
      local ke, pe, te = compute_total_energy(bodies)
      print(string.format("%d\t%.3e\t%.3e\t%.3e", step, ke, pe, te))
    end
  end
  
  local ke, pe, te = compute_total_energy(bodies)
  print(string.format("Final:\t%.3e\t%.3e\t%.3e", ke, pe, te))
end

local n_bodies, timestep, num_steps = parse_args()
local bodies = init_bodies(n_bodies)
simulate(bodies, timestep, num_steps)
